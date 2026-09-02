/* Interpreter bring-up test.
 *
 * Loads the image, binds every GOT slot to the stub page, and interprets from
 * the ARM entry stub. The HLE set here is the minimum the Unicorn reference
 * (../../loader/run_boz.py) showed is needed to get through early startup.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "guest.h"
#include "s3e_loader.h"
#include "s3e_files.h"
#include "s3e_config.h"
#ifdef __SWITCH__
#include "gl_thunks.h"
#endif

#define STACK_BASE 0x20000000u
#define STACK_SIZE (1u << 20)
#define HEAP_BASE  0x60000000u
#define BOZ_BUILD_LABEL "interp-fast-r21 " __DATE__ " " __TIME__
/* The allocator is a pure bump allocator and free() reclaims nothing, so
 * exhaustion is self-inflicted and the game does not NULL-check malloc -- it
 * runs a C++ constructor on the result and faults writing the vtable. The
 * reference sidesteps this the same way, defaulting to a 1 GB heap when its
 * free lists are disabled for tracing. Real reclamation is still needed for
 * the device, where applet mode leaves only ~448 MB in total. */
#ifdef __SWITCH__
#define HEAP_SIZE  (320u << 20)
#else
#define HEAP_SIZE  (1024u << 20)
#endif

static uint8_t *g_stack, *g_heap;
static uint32_t g_brk = HEAP_BASE;
static S3eImage g_img;
static int g_calls, g_shown;
static int g_hook_hits[3];
static uint32_t g_ext_stub;          /* guest addr of a "return 0" stub */

static void gstr(const GuestMem *m, uint32_t addr, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) {
        uint32_t ch;
        if (!guest_ld8(m, addr + (uint32_t)i, &ch) || ch == 0)
            break;
        out[i] = (char)ch;
    }
    out[i] = 0;
}

/* Allocation sizes live in a side table rather than a header word, so guest
 * addresses stay byte-identical to the Unicorn reference's bump allocator and
 * the differential keeps working. Addresses are handed out strictly
 * increasing, so this stays sorted and binary-searches. */
typedef struct {
    uint32_t addr, size;
    uint32_t next_free;             /* encoded index+1; 0 terminates a list */
    uint8_t in_use;
} GuestAlloc;

static GuestAlloc *g_allocs;
static uint32_t g_alloc_n, g_alloc_cap;
static uint32_t g_free_head[4096];  /* exact-size reuse, hash collisions chained */
static uint32_t g_live_bytes, g_peak_bytes;

static void galloc_record(uint32_t addr, uint32_t size) {
    if (g_alloc_n == g_alloc_cap) {
        uint32_t cap = g_alloc_cap ? g_alloc_cap * 2u : 4096u;
        void *p = realloc(g_allocs, cap * sizeof *g_allocs);
        if (!p)
            return;                     /* size lookup degrades, not fatal */
        g_allocs = p;
        g_alloc_cap = cap;
    }
    g_allocs[g_alloc_n].addr = addr;
    g_allocs[g_alloc_n].size = size;
    g_allocs[g_alloc_n].next_free = 0;
    g_allocs[g_alloc_n].in_use = 1;
    g_alloc_n++;
}

static int32_t galloc_index(uint32_t addr) {
    uint32_t lo = 0, hi = g_alloc_n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (g_allocs[mid].addr == addr)
            return (int32_t)mid;
        if (g_allocs[mid].addr < addr)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return -1;
}

/* 0 for a pointer this allocator never handed out -- the reference treats an
 * unknown pointer the same way and copies nothing. */
static uint32_t galloc_size(uint32_t addr) {
    int32_t i = galloc_index(addr);
    return i >= 0 ? g_allocs[i].size : 0;
}

static uint32_t galloc(uint32_t n) {
    uint32_t p;
    n = (n + 15u) & ~15u;
#ifdef __SWITCH__
    /* The host oracle deliberately keeps monotonically increasing addresses.
     * Hardware cannot afford that: reclaim an exact-size block without moving
     * any live allocation, so guest pointers remain stable and fragmentation
     * cannot make realloc corrupt neighbouring objects. */
    if (!n)
        n = 16;
    {
        uint32_t bucket = (n >> 4) & 4095u;
        uint32_t *link = &g_free_head[bucket];
        while (*link) {
            uint32_t i = *link - 1u;
            if (g_allocs[i].size == n) {
                *link = g_allocs[i].next_free;
                g_allocs[i].next_free = 0;
                g_allocs[i].in_use = 1;
                g_live_bytes += n;
                if (g_live_bytes > g_peak_bytes)
                    g_peak_bytes = g_live_bytes;
                return g_allocs[i].addr;
            }
            link = &g_allocs[i].next_free;
        }
    }
#endif
    if (g_brk + n > HEAP_BASE + HEAP_SIZE) {
        static int shown;
        if (shown < 3) {
            printf("  [OOM  ] malloc(%u) failed, %u MB used\n", (unsigned)n,
                   (unsigned)((g_brk - HEAP_BASE) >> 20));
            shown++;
        }
        return 0;
    }
    p = g_brk;
    g_brk += n;
    galloc_record(p, n);
    g_live_bytes += n;
    if (g_live_bytes > g_peak_bytes)
        g_peak_bytes = g_live_bytes;
    return p;
}

static void gfree(uint32_t addr) {
#ifdef __SWITCH__
    int32_t si;
    uint32_t i, bucket;
    if (!addr)
        return;
    si = galloc_index(addr);
    if (si < 0)
        return;                         /* tolerate foreign/old SDK pointers */
    i = (uint32_t)si;
    if (!g_allocs[i].in_use)
        return;                         /* double-free: do not poison the list */
    g_allocs[i].in_use = 0;
    if (g_live_bytes >= g_allocs[i].size)
        g_live_bytes -= g_allocs[i].size;
    bucket = (g_allocs[i].size >> 4) & 4095u;
    g_allocs[i].next_free = g_free_head[bucket];
    g_free_head[bucket] = i + 1u;
#else
    (void)addr;                         /* preserve oracle address ordering */
#endif
}

/* realloc has to PRESERVE the old contents. Returning a fresh block without
 * copying silently zeroes whatever was there; the game grows arrays of object
 * pointers through here, so every growth left holes, and a later linear scan
 * dereferenced a null entry before reaching the element count. */
static uint32_t grealloc(GuestMem *mem, uint32_t old, uint32_t n) {
    uint32_t keep = galloc_size(old);
    uint32_t aligned = (n + 15u) & ~15u;
    uint32_t p;
    if (!old)
        return galloc(n);
    if (!n) {
        gfree(old);
        return 0;
    }
    if (keep == aligned)
        return old;
    p = galloc(n);
    if (p && old && keep) {
        void *dst, *src;
        if (keep > n)
            keep = n;
        dst = guest_ptr(mem, p, keep);
        src = guest_ptr(mem, old, keep);
        if (dst && src)
            memcpy(dst, src, keep);
    }
    if (p)
        gfree(old);
    return p;
}

static const char *slot_name(uint32_t idx) {
    int32_t si = (idx < g_img.got_count) ? g_img.got_import[idx] : -1;
    return si >= 0 ? g_img.import_names[si] : "<unnamed>";
}

/* --------------------------------------------------------------- handlers */

static void hle_trace(GuestCpu *cpu, GuestMem *mem, void *user) {
    char buf[160];
    (void)user;
    gstr(mem, cpu->r[0], buf, sizeof buf);
    if (buf[0])
        printf("  [guest] %s\n", buf);
    else if (cpu->r[1]) {
        gstr(mem, cpu->r[1], buf, sizeof buf);
        printf("  [guest] %s\n", buf);
    }
    cpu->r[0] = 0;
}

static void hle_malloc(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = galloc(cpu->r[0]);
}

static void hle_realloc(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    cpu->r[0] = grealloc(mem, cpu->r[0], cpu->r[1]);
}

static void hle_free(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    gfree(cpu->r[0]);
    cpu->r[0] = 0;
}

/* s3eExtGetHash(hash, out_table, bytes): a zero return makes the caller take
 * the "extension missing" path and then call the table entries anyway, so the
 * table has to be filled with something callable. */
static void hle_extgethash(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t i;
    (void)user;
    for (i = 0; i + 4 <= cpu->r[2]; i += 4)
        guest_st32(mem, cpu->r[1] + i, g_ext_stub);
    cpu->r[0] = 1;
}

/* Callers dereference the result without a NULL check. */
static void hle_devstring(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t s;
    (void)user;
    if (!s) {
        s = galloc(8);
        guest_st32(mem, s, 0x6E6B6E75u);      /* "unkn" */
        guest_st32(mem, s + 4, 0x006E776Fu);  /* "own\0" */
    }
    cpu->r[0] = s;
}

/* The frontend's low-memory dialog is selected at RVA 0x188662. It asks
 * s3eDeviceGetInt(6) for the device-memory figure, then compares it with
 * [GAME] FrontendMemoryWarningLevel (25,000,000 by default). The generic HLE
 * returned zero for every property, so the warning was guaranteed even while
 * the guest heap had hundreds of MB free. Property 0 is the OS id; 0x12 is
 * Android, matching this deployed Android image. Report the emulated heap for
 * property 6 so both sides of the original check receive coherent values. */
static void hle_device_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0], value;
    (void)mem; (void)user;
    switch (prop) {
    case 0:  value = 0x12u;      break;  /* S3E_DEVICE_OS: Android */
    case 6:  value = HEAP_SIZE;  break;  /* device memory, bytes */
    default: value = 0;          break;
    }
    if (prop < 32 && !(seen & (1u << prop))) {
        seen |= 1u << prop;
        printf("  [dev  ] GetInt(%u) -> %u\n",
               (unsigned)prop, (unsigned)value);
    }
    cpu->r[0] = value;
}

/* NUL-terminated store into guest memory; fails closed on an unmapped byte
 * rather than reporting a buffer it did not fill. */
static int gputs(GuestMem *m, uint32_t addr, const char *s) {
    uint32_t i;
    for (i = 0;; i++) {
        if (!guest_st8(m, addr + i, (uint8_t)s[i]))
            return 0;
        if (!s[i])
            return 1;
    }
}

/* Each key is logged once, with the answer it got, so the next unhandled one
 * names itself -- that is how LowMemoryDevice was tracked down. */
static void cfg_log_once(const char *tag, const char *sect, const char *key,
                         const char *val) {
    static char seen[64][56];
    static int nseen;
    const char *parked;
    char id[56];
    int i;
    snprintf(id, sizeof id, "%s|%s|%s", tag, sect, key);
    for (i = 0; i < nseen; i++)
        if (!strcmp(seen[i], id))
            return;
    if (nseen >= 64)
        return;
    snprintf(seen[nseen], sizeof seen[0], "%s", id);
    nseen++;
    /* A parked key prints the value it would have had. That is the bisection
     * data: it says which "not set" answers are a decision from r18 and which
     * are simply keys nobody has looked at. */
    parked = val ? NULL : s3e_config_parked(sect, key);
    if (parked)
        printf("  [%s] [%s] %s -> not set (PARKED, would be %s)\n",
               tag, sect, key, parked);
    else
        printf("  [%s] [%s] %s -> %s\n", tag, sect, key,
               val ? val : "not set");
}

/* s3eConfigGetInt(section, key, out). Answers from the shared table in
 * jit/s3e_config.h; anything not in it stays "not set" so the game keeps its
 * own defaults. */
static void hle_configint(GuestCpu *cpu, GuestMem *mem, void *user) {
    char sect[32], key[48];
    const char *val;
    (void)user;
    gstr(mem, cpu->r[0], sect, sizeof sect);
    gstr(mem, cpu->r[1], key, sizeof key);
    val = s3e_config_get(sect, key);
    if (val) {
        char *end;
        long n = strtol(val, &end, 0);
        if (end != val && !*end && guest_st32(mem, cpu->r[2], (uint32_t)n)) {
            cfg_log_once("cfg  ", sect, key, val);
            cpu->r[0] = 0;                    /* S3E_RESULT_SUCCESS */
            return;
        }
    }
    cfg_log_once("cfg  ", sect, key, NULL);
    cpu->r[0] = 1;                            /* not set: use game defaults */
}

/* s3eConfigGetString(section, key, out), same table. Returning 0 without
 * filling the buffer would tell the caller a string is there when it is not,
 * so every failure path answers 1. */
static void hle_configstr(GuestCpu *cpu, GuestMem *mem, void *user) {
    char sect[32], key[48];
    const char *val;
    (void)user;
    gstr(mem, cpu->r[0], sect, sizeof sect);
    gstr(mem, cpu->r[1], key, sizeof key);
    val = s3e_config_get(sect, key);
    if (val && gputs(mem, cpu->r[2], val)) {
        cfg_log_once("cfgs ", sect, key, val);
        cpu->r[0] = 0;
        return;
    }
    cfg_log_once("cfgs ", sect, key, NULL);
    cpu->r[0] = 1;
}

/* The app routes allocations through a manager object it builds itself,
 * whose three function pointers are never populated here -- its wrappers
 * branch through NULL. A port replaces the guest allocator regardless, so
 * intercept the wrappers directly. RVAs from the Unicorn bring-up.
 */
#define RVA_MGR_MALLOC  0x34c1a8u
#define RVA_MGR_FREE    0x34c1c4u
#define RVA_MGR_REALLOC 0x34c1e0u

static void hook_malloc(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    g_hook_hits[0]++;
    cpu->r[0] = galloc(cpu->r[0]);
}

static void hook_realloc(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    g_hook_hits[1]++;
    cpu->r[0] = grealloc(mem, cpu->r[0], cpu->r[1]);
}

static void hook_free(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    g_hook_hits[2]++;
    gfree(cpu->r[0]);
    cpu->r[0] = 0;
}

/* Verified ARM EABI memory primitives in this exact image. These two routines
 * dominate interpreted startup and frame time; executing their byte/word loops
 * as guest instructions is pure overhead. The normal non-observe hook return
 * preserves ARM/Thumb interworking through LR. */
#define RVA_NATIVE_MEMCPY 0x365dc0u
#define RVA_NATIVE_MEMSET 0x3664f4u
static uint64_t g_fast_mem_bytes[2];
static uint32_t g_fast_mem_hits[2];

/* A non-observe hook never runs the routine it replaces: interp.c branches
 * through LR the moment the handler returns. So bailing out on a rejected
 * range would leave the destination untouched and the guest running on --
 * trading a clean GUEST_FAULT for silent corruption. Fall back to the checked
 * byte accessors instead: they copy whatever really is mapped and name the
 * first address that is not. */
static void slow_copy(GuestMem *mem, uint32_t dst, uint32_t src, uint32_t len) {
    uint32_t i, byte;
    for (i = 0; i < len; i++)
        if (!guest_ld8(mem, src + i, &byte) ||
            !guest_st8(mem, dst + i, byte)) {
            printf("  [fast ] memcpy unmapped at +%u (dst=%08x src=%08x "
                   "len=%u)\n", (unsigned)i, (unsigned)dst, (unsigned)src,
                   (unsigned)len);
            return;
        }
}

static void slow_fill(GuestMem *mem, uint32_t dst, uint32_t byte, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len; i++)
        if (!guest_st8(mem, dst + i, byte)) {
            printf("  [fast ] memset unmapped at +%u (dst=%08x len=%u)\n",
                   (unsigned)i, (unsigned)dst, (unsigned)len);
            return;
        }
}

static void hook_native_memcpy(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t dst = cpu->r[0], src = cpu->r[1], len = cpu->r[2];
    void *d;
    const void *s;
    (void)user;
    if (len) {
        d = guest_ptr(mem, dst, len);
        s = guest_ptr(mem, src, len);
        if (!d || !s)
            slow_copy(mem, dst, src, len);
        else
            memmove(d, s, len);
    }
    cpu->r[0] = dst;
    g_fast_mem_hits[0]++;
    g_fast_mem_bytes[0] += len;
}

static void hook_native_memset(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t dst = cpu->r[0], value = cpu->r[1], len = cpu->r[2];
    void *d;
    (void)user;
    if (len) {
        d = guest_ptr(mem, dst, len);
        if (!d)
            slow_fill(mem, dst, value & 0xffu, len);
        else
            memset(d, (int)(value & 0xffu), len);
    }
    cpu->r[0] = dst;
    g_fast_mem_hits[1]++;
    g_fast_mem_bytes[1] += len;
}

/* The image-conversion registry is built by RVA 0x2720d4. It owns four tiny
 * format handlers, then the dispatcher at RVA 0x27109c looks one up and
 * immediately calls vtable[5].
 *
 * On hardware a later memset clears these objects while the registry continues
 * to point at them. That produced the fault at RVA 0x2710bc (load from address
 * 0x14). Repair only a verified registry singleton, at the last safe point
 * before the dispatcher dereferences it. This is
 * intentionally narrower than a general NULL-vtable workaround: another
 * broken object must still fault and identify itself.
 */
#define RVA_IMAGE_HANDLER_READY 0x2710a0u
#define RVA_IMAGE_HANDLER_SLOTS 0x49fd88u

static void watch_image_handler(GuestCpu *cpu, GuestMem *mem, void *user) {
    static int repairs;
    static const uint32_t vtable_rva[4] = {
        0x409fb0u, 0x409e10u, 0x409f78u, 0x409dd8u
    };
    uint32_t registered = 0, vtable = 0, format = 0;
    int slot = -1, expected_slot = -1, i;
    (void)user;

    if (!cpu->r[0])
        return;
    if (cpu->r[6])
        guest_ld16(mem, cpu->r[6], &format);

    for (i = 0; i < 4; i++) {
        if (guest_ld32(mem, g_img.load_base + RVA_IMAGE_HANDLER_SLOTS +
                      4u * (uint32_t)i, &registered) &&
            registered == cpu->r[0]) {
            slot = i;
            break;
        }
    }
    if (slot < 0 || !guest_ld32(mem, cpu->r[0], &vtable) || vtable != 0)
        return;

    if (format == 0x20u || format == 0x21u) expected_slot = 0;
    else if (format == 0x22u)                expected_slot = 1;
    else if (format == 0x27u)                expected_slot = 2;
    else if (format == 0x2bu || format == 0x34u || format == 0x35u)
                                                expected_slot = 3;
    if (slot != expected_slot) {
        printf("  [img  ] slot%d has NULL vtable for unexpected format 0x%x; "
               "leaving it to fault\n", slot, (unsigned)format);
        return;
    }

    guest_st32(mem, registered, g_img.load_base + vtable_rva[slot]);
    if (slot == 3)
        guest_st8(mem, registered + 4, 0);   /* slot3 constructor state */
    repairs++;
    printf("  [img  ] restored slot%d handler %08x for format 0x%x "
           "at lookup #%d\n", slot, (unsigned)registered,
           (unsigned)format, repairs);
}

/* RVA 0x118be8 is the positive half of an angle-normalisation loop:
 *
 *     while (angle > 2*pi) angle -= 2*pi;
 *
 * A bad upstream value made this execute ~44 million guest iterations on the
 * loading screen. fmodf is equivalent for the intended range and prevents one
 * malformed frame value from consuming minutes of interpreter time. This is
 * deliberately tied to the verified game RVA rather than changing general VFP
 * semantics. The hook is observe-mode; moving PC makes the interpreter execute
 * the first instruction after the loop in the same step. */
#define RVA_FAST_ANGLE_NORMALIZE 0x118be8u
static void fast_angle_normalize(GuestCpu *cpu, GuestMem *mem, void *user) {
    static int shown;
    uint32_t bits;
    float angle, reduced;
    const float two_pi = 6.283185482025146484375f;
    (void)user;
    if (!guest_ld32(mem, cpu->r[4] + 0x58u, &bits))
        return;
    memcpy(&angle, &bits, sizeof angle);
    if (!(angle > two_pi) || !isfinite(angle))
        return;
    reduced = fmodf(angle, two_pi);
    if (reduced == 0.0f)
        reduced = two_pi;
    memcpy(&bits, &reduced, sizeof bits);
    guest_st32(mem, cpu->r[4] + 0x58u, bits);
    cpu->r[GUEST_PC] = g_img.load_base + 0x118c04u;
    if (shown < 4) {
        printf("  [perf ] fast angle normalize %.3g -> %.6f\n",
               (double)angle, (double)reduced);
        shown++;
    }
}

/* ---- file layer: the s3eFile* imports, backed by jit/s3e_files.c -------
 * Argument orders are Marmalade's, taken from the Unicorn reference so both
 * harnesses resolve the same bytes. Note s3eFileRead/Write take the handle in
 * r3, not r0. */
static int g_file_shown;

static int g_file_failed, g_exists_failed;

static void hle_file_open(GuestCpu *cpu, GuestMem *mem, void *user) {
    char fn[256], md[16];
    (void)user;
    gstr(mem, cpu->r[0], fn, sizeof fn);
    gstr(mem, cpu->r[1], md, sizeof md);
    cpu->r[0] = s3e_vfs_open(fn, md);
    /* Failures are the interesting half and were being hidden by the cap: the
     * menu renders every quad with the engine's 2x2 default texture, so the
     * question is whether the game asks for its real art and is refused. Cap
     * the successes, never the refusals. */
    if (!cpu->r[0]) {
        g_file_failed++;
        if (g_file_failed <= 200)
            printf("  [file ] MISS %s (%s)\n", fn, md);
        else if (g_file_failed == 201)
            printf("  [file ] ... further misses not listed\n");
    } else if (g_file_shown < 40) {
        printf("  [file ] open %s (%s) -> %08x\n", fn, md, (unsigned)cpu->r[0]);
        g_file_shown++;
    }
}

static void hle_file_exists(GuestCpu *cpu, GuestMem *mem, void *user) {
    char fn[256];
    (void)user;
    gstr(mem, cpu->r[0], fn, sizeof fn);
    cpu->r[0] = (uint32_t)s3e_vfs_exists(fn);
    /* The engine probes before it opens, so a negative probe means the asset is
     * never even requested -- a different failure from an open that is refused. */
    if (!cpu->r[0]) {
        g_exists_failed++;
        if (g_exists_failed <= 60)
            printf("  [file ] ABSENT %s\n", fn);
    }
}

static void hle_file_read(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t n = cpu->r[1] * cpu->r[2], got = 0;
    void *dst = n ? guest_ptr(mem, cpu->r[0], n) : NULL;
    (void)user;
    if (dst)
        got = s3e_vfs_read(cpu->r[3], dst, n);
    cpu->r[0] = cpu->r[1] ? got / cpu->r[1] : 0;
}

static void hle_file_write(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t n = cpu->r[1] * cpu->r[2], put = 0;
    void *src = n ? guest_ptr(mem, cpu->r[0], n) : NULL;
    (void)user;
    if (src)
        put = s3e_vfs_write(cpu->r[3], src, n);
    cpu->r[0] = cpu->r[1] ? put / cpu->r[1] : 0;
}

static void hle_file_seek(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = (uint32_t)s3e_vfs_seek(cpu->r[0], (int32_t)cpu->r[1], cpu->r[2]);
}

static void hle_file_tell(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = s3e_vfs_tell(cpu->r[0]);
}

static void hle_file_size(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = s3e_vfs_size(cpu->r[0]);
}

static void hle_file_close(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    s3e_vfs_close(cpu->r[0]);
    cpu->r[0] = 0;
}

static void hle_file_error(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = (uint32_t)s3e_vfs_error();
}

static void hle_file_getchar(GuestCpu *cpu, GuestMem *mem, void *user) {
    unsigned char b;
    (void)mem; (void)user;
    cpu->r[0] = (s3e_vfs_read(cpu->r[0], &b, 1) == 1) ? (uint32_t)b : 0xFFFFFFFFu;
}

static void hle_file_delete(GuestCpu *cpu, GuestMem *mem, void *user) {
    char fn[256];
    (void)user;
    gstr(mem, cpu->r[0], fn, sizeof fn);
    s3e_vfs_delete(fn);
    cpu->r[0] = 0;
}

static void hle_file_mkdir(GuestCpu *cpu, GuestMem *mem, void *user) {
    char fn[256];
    (void)user;
    gstr(mem, cpu->r[0], fn, sizeof fn);
    s3e_vfs_mkdir(fn);
    cpu->r[0] = 0;
}

static void hle_file_flush(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = 0;
}

static void hle_zero(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = 0;
}

/* ---- callbacks --------------------------------------------------------
 * s3eXxxRegister(id, fn, userData) hands the runtime a guest function to call
 * back into. The engine registers a set of ids and a UI layer later
 * re-registers the same ones, so the LAST registration for a (kind, id) wins.
 *
 * Delivery happens only at s3eDeviceYield. That is where the guest expects the
 * runtime to take over, so it is the one safe place to re-enter guest code --
 * guest_call() saves and restores the interrupted state around it. (The
 * Unicorn reference cannot do this from inside a hook and has to stop the
 * emulator and deliver from its driver loop; an interpreter just recurses.)
 */
#define MAX_CBS 64

typedef struct {
    char     kind[28];          /* import name minus the trailing "Register" */
    uint32_t id, fn, user;
    int      used;
} Callback;

static Callback g_cbs[MAX_CBS];
static int g_cb_n, g_yields;
static struct { int slot; uint32_t sysdata; } g_cb_queue[32];
static int g_cb_queue_n;

static int ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

static void cb_kind(const char *nm, char *out, size_t n) {
    size_t len = strlen(nm);
    if (ends_with(nm, "UnRegister"))
        len -= 10;
    else if (ends_with(nm, "Register"))
        len -= 8;
    if (len >= n)
        len = n - 1;
    memcpy(out, nm, len);
    out[len] = 0;
}

static int cb_find(const char *kind, uint32_t id) {
    int i;
    for (i = 0; i < g_cb_n; i++)
        if (g_cbs[i].used && g_cbs[i].id == id && !strcmp(g_cbs[i].kind, kind))
            return i;
    return -1;
}

static void hle_register(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t idx = (uint32_t)(uintptr_t)user;
    char kind[28];
    int i;
    (void)mem;
    cb_kind(slot_name(idx), kind, sizeof kind);
    i = cb_find(kind, cpu->r[0]);
    if (i < 0 && g_cb_n < MAX_CBS)
        i = g_cb_n++;
    if (i >= 0) {
        memcpy(g_cbs[i].kind, kind, sizeof kind);
        g_cbs[i].id = cpu->r[0];
        g_cbs[i].fn = cpu->r[1];
        g_cbs[i].user = cpu->r[2];
        g_cbs[i].used = 1;
        printf("  [cb   ] %-18s id=%-3u fn=%08x user=%08x\n", kind,
               (unsigned)cpu->r[0], (unsigned)cpu->r[1], (unsigned)cpu->r[2]);
    }
    cpu->r[0] = 0;                          /* S3E_RESULT_SUCCESS */
}

static void hle_unregister(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t idx = (uint32_t)(uintptr_t)user;
    char kind[28];
    int i;
    (void)mem;
    cb_kind(slot_name(idx), kind, sizeof kind);
    i = cb_find(kind, cpu->r[0]);
    if (i >= 0)
        g_cbs[i].used = 0;
    cpu->r[0] = 0;
}

/* Queue one for the next yield rather than calling straight away: a callback
 * fired from the middle of an unrelated import would re-enter the guest at a
 * point it does not expect. */
static int cb_queue(const char *kind, uint32_t id, uint32_t sysdata) {
    int i = cb_find(kind, id);
    if (i < 0 || g_cb_queue_n >= (int)(sizeof g_cb_queue / sizeof *g_cb_queue))
        return 0;
    g_cb_queue[g_cb_queue_n].slot = i;
    g_cb_queue[g_cb_queue_n].sysdata = sysdata;
    g_cb_queue_n++;
    return 1;
}

static void cb_pump(void);      /* defined after `g`, which it re-enters */

static void hle_device_yield(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    g_yields++;
    if (g_yields <= 3 || (g_yields % 1000) == 0)
        printf("  [yield] #%d (%d callbacks registered)\n", g_yields, g_cb_n);
    cb_pump();
    cpu->r[0] = 0;
}

/* ---- surface geometry -------------------------------------------------
 * 480x320 RGB565, confirmed twice by measurement (vertical-coherence scoring
 * over real rendered pixels), not guessed. The game writes past the nominal
 * end of the surface, so the region is over-allocated rather than sized
 * exactly. Declared here because the input code below needs the dimensions to
 * place a tap at the screen centre. */
/* Panel geometry and the letterbox the 480x320 surface is drawn into. Declared
 * here because the pointer code maps touches back through it, and that runs
 * well before the framebuffer is created. */
#define FB_W 1280u
#define FB_H 720u
#define VIEW_W 1080u
#define VIEW_X ((FB_W - VIEW_W) / 2u)

static PadState g_pad;
static int g_quit;
static int g_saw_real_input;         /* once true, the synthetic tap stands down */

#define SURF_BASE    0x40000000u
#define SCREEN_W     480u
#define SCREEN_H     320u
#define SURF_BPP     2u
#define SURF_PIXTYPE 0x422u
#define SURF_FRAME   (SCREEN_W * SCREEN_H * SURF_BPP)
#define SURF_BYTES   (((SURF_FRAME + 0xFFFu) & ~0xFFFu) + (16u << 20))

/* ---- synthetic input --------------------------------------------------
 * The game both POLLS (s3ePointerGetState/GetX/GetY each frame) and registered
 * a pointer callback, so a tap has to arrive on both paths -- driving only the
 * callback, as the Unicorn reference does, can be missed entirely by a poller.
 *
 * Frame-driven rather than instruction-driven so the sequence is deterministic
 * and two runs stay comparable. BOZ_TAP sets the frame to tap on (0 disables),
 * which allows an A/B run without rebuilding.
 *
 * Event struct is {int32 button, pressed, x, y} -- the layout the reference
 * harness uses, matching Marmalade's s3ePointerEvent.
 */
#define TAP_DEFAULT_FRAME 500       /* well after output has stabilised */
#define TAP_HOLD_UPDATES  6       /* Updates held down before release */

/* s3ePointerState. These are DISCRETE values, not bit flags -- verified
 * against the game's own comparisons:
 *   RVA 0x08f5a0: subs r3,r0,#1 / cmp r3,#1 / movls  => "touching" iff
 *                 state is in {1,2}, so DOWN and PRESSED are 1 and 2 and
 *                 OR-ing them (3) would read as NOT touching.
 *   RVA 0x0e5034: cmp r0,#1                => exact test for DOWN.
 *   RVA 0x0e4b5e: cmp r0,#0 / beq          => UP is 0.
 * RELEASED is not compared anywhere in the image; 3 is the documented SDK
 * value and nothing contradicts it. */
#define S3E_PTR_UP        0
#define S3E_PTR_DOWN      1
#define S3E_PTR_PRESSED   2
#define S3E_PTR_RELEASED  3

static int g_tap_frame = -1;        /* resolved on first present */
static int g_tap_x, g_tap_y;
static int g_ptr_state = S3E_PTR_UP;
static uint32_t g_ev_press, g_ev_release, g_ev_motion;
static int g_tap_done, g_tap_armed, g_tap_step;

static void tap_fire(GuestMem *mem, uint32_t *buf, int pressed) {
    if (!*buf)
        *buf = galloc(16);
    if (!*buf)
        return;
    guest_st32(mem, *buf + 0, 0);                   /* m_Button */
    guest_st32(mem, *buf + 4, (uint32_t)pressed);   /* m_Pressed */
    guest_st32(mem, *buf + 8, (uint32_t)g_tap_x);
    guest_st32(mem, *buf + 12, (uint32_t)g_tap_y);
    {   /* with real input this fires on every touch, so log sparsely */
        static int shown;
        if (shown < 12 || !g_saw_real_input) {
            shown++;
            printf("  [tap  ] pointer callback pressed=%d at (%d,%d)\n",
                   pressed, g_tap_x, g_tap_y);
        }
    }
    if (!cb_queue("s3ePointer", 0, *buf))
        printf("  [tap  ] no s3ePointer id=0 callback registered\n");
}

static void motion_fire(GuestMem *mem, int x, int y) {
    static int shown;
    if (!g_ev_motion)
        g_ev_motion = galloc(8);
    if (!g_ev_motion)
        return;
    guest_st32(mem, g_ev_motion + 0, (uint32_t)x);
    guest_st32(mem, g_ev_motion + 4, (uint32_t)y);
    cb_queue("s3ePointer", 1, g_ev_motion);
    if (shown < 8) {
        printf("  [move ] pointer motion callback at (%d,%d)\n", x, y);
        shown++;
    }
}

/* Arm on a frame count so the tap lands after output has stabilised; the
 * sequence itself then advances on s3ePointerUpdate, which is the SDK's
 * contract -- transient states (PRESSED/RELEASED) are visible until the next
 * Update, not until the next frame. The game calls Update exactly once per
 * frame from RVA 0x0c6506, so in practice the two coincide, but driving it
 * from Update is what a real s3e implementation does. */
static void tap_arm(int frame) {
    if (g_tap_frame < 0) {
        const char *e = getenv("BOZ_TAP");
        g_tap_frame = e ? atoi(e) : TAP_DEFAULT_FRAME;
        g_tap_x = (int)(SCREEN_W / 2);
        g_tap_y = (int)(SCREEN_H / 2);
        printf("  [tap  ] scheduled for frame %d (0 = disabled)\n", g_tap_frame);
    }
    if (g_tap_frame > 0 && !g_tap_armed && frame >= g_tap_frame) {
        g_tap_armed = 1;
        printf("  [tap  ] armed at frame %d\n", frame);
    }
}

/* ---- real input -------------------------------------------------------
 *
 * This is a touchscreen game, so in handheld mode the touchscreen drives the
 * pointer directly. Docked, the left stick moves a cursor and A presses it.
 *
 * The coordinate mapping has to invert the viewport rescale: the game believes
 * the screen is 480x320 and we draw it into a 1080x720 box at x=100, so a
 * touch at screen (sx, sy) is game ((sx - 100) * 480/1080, sy * 320/720).
 * Both spaces have their origin at the top left, so no flip is involved --
 * unlike the GL viewport, which measures from the bottom. */
static int g_touch_ready;
static int g_real_down;              /* contact state on the previous update */
static int g_cur_x = (int)(SCREEN_W / 2), g_cur_y = (int)(SCREEN_H / 2);

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int input_poll(int *px, int *py) {
    HidTouchScreenState ts;
    u64 held;

    if (!g_touch_ready) {
        hidInitializeTouchScreen();
        g_touch_ready = 1;
    }
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        *px = clampi(((int)ts.touches[0].x - (int)VIEW_X) * (int)SCREEN_W
                     / (int)VIEW_W, 0, (int)SCREEN_W - 1);
        *py = clampi((int)ts.touches[0].y * (int)SCREEN_H / (int)FB_H,
                     0, (int)SCREEN_H - 1);
        g_cur_x = *px;
        g_cur_y = *py;
        return 1;
    }

    /* Docked: no touchscreen, so drive a cursor with the left stick. The
     * divisor turns a full deflection into a few pixels per frame. */
    padUpdate(&g_pad);
    {
        HidAnalogStickState st = padGetStickPos(&g_pad, 0);
        if (st.x > 6000 || st.x < -6000)
            g_cur_x = clampi(g_cur_x + st.x / 4000, 0, (int)SCREEN_W - 1);
        if (st.y > 6000 || st.y < -6000)
            g_cur_y = clampi(g_cur_y - st.y / 4000, 0, (int)SCREEN_H - 1);
    }
    held = padGetButtons(&g_pad);
    *px = g_cur_x;
    *py = g_cur_y;
    return (held & HidNpadButton_A) ? 1 : 0;
}

/* void s3ePointerUpdate(void) -- verified: at its only call site (RVA
 * 0x0c6506) r0 is overwritten by `ldr r0,[r3]` immediately after, so the
 * return value is discarded. */
static void hle_ptr_update(GuestCpu *cpu, GuestMem *mem, void *user) {
    static int last_motion_x = -1, last_motion_y = -1;
    int x = 0, y = 0, down;
    (void)user;

    down = input_poll(&x, &y);
    if (down || g_real_down) {
        g_tap_x = x;
        g_tap_y = y;
        if (!g_saw_real_input) {
            g_saw_real_input = 1;
            printf("  [in   ] real input active -- synthetic tap stands down\n");
        }
        if (down && !g_real_down) {
            g_ptr_state = S3E_PTR_PRESSED;
            tap_fire(mem, &g_ev_press, 1);
        } else if (down) {
            g_ptr_state = S3E_PTR_DOWN;
        } else {
            g_ptr_state = S3E_PTR_RELEASED;
            tap_fire(mem, &g_ev_release, 0);
        }
        /* id 1 is s3ePointerMotionEvent {x,y}. The old port only updated the
         * polling getters, so code driven by the registered motion callback
         * saw the press at one position but no drag/movement at all. */
        if (down && (x != last_motion_x || y != last_motion_y)) {
            motion_fire(mem, x, y);
            last_motion_x = x;
            last_motion_y = y;
        }
        g_real_down = down;
        cpu->r[0] = 0;
        return;
    }
    g_real_down = 0;
    if (g_saw_real_input)
        g_ptr_state = S3E_PTR_UP;

    if (!g_saw_real_input && g_tap_armed && !g_tap_done) {
        g_tap_step++;
        if (g_tap_step == 1) {
            g_ptr_state = S3E_PTR_PRESSED;
            tap_fire(mem, &g_ev_press, 1);
        } else if (g_tap_step <= 1 + TAP_HOLD_UPDATES) {
            g_ptr_state = S3E_PTR_DOWN;
        } else if (g_tap_step == 2 + TAP_HOLD_UPDATES) {
            g_ptr_state = S3E_PTR_RELEASED;
            tap_fire(mem, &g_ev_release, 0);
        } else {
            g_ptr_state = S3E_PTR_UP;
            g_tap_done = 1;
            printf("  [tap  ] sequence complete after %d updates\n", g_tap_step);
        }
        printf("  [tap  ] update #%d -> state %d\n", g_tap_step, g_ptr_state);
    }
    cpu->r[0] = 0;                      /* ignored by the caller */
}

/* The game polls buttons 0, 1 and 2 (RVAs 0x0e4b5e, 0x0e4c20, 0x0e4c08).
 * A single-finger tap is button 0 only; anything else must read as UP or the
 * game sees three simultaneous contacts. */
static void hle_ptr_getstate(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = (cpu->r[0] == 0) ? (uint32_t)g_ptr_state : (uint32_t)S3E_PTR_UP;
}

static void hle_ptr_getx(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = (uint32_t)g_tap_x;
}

static void hle_ptr_gety(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = (uint32_t)g_tap_y;
}

/* s3ePointerProperty. The game queries 4 (MULTI_TOUCH_AVAILABLE, compared
 * ==1 at RVA 0x0dd4d6/0x0ddcd4) and 2 (TYPE, compared ==1 i.e. MOUSE, at RVA
 * 0x149f7e). Answering TYPE=2 (STYLUS) takes the touch path rather than the
 * mouse path; returning 0 (INVALID) risks a "no pointer" branch. A wrong
 * answer here is exactly the sort of capability gate that silently steers the
 * game onto a degraded path, so every property asked for is logged. */
static void hle_ptr_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0], v;
    (void)mem; (void)user;
    switch (prop) {
    case 0:  v = 1; break;              /* AVAILABLE */
    case 2:  v = 2; break;              /* TYPE = STYLUS (touchscreen) */
    case 3:  v = 2; break;              /* STYLUS_TYPE = FINGER */
    case 4:  v = 0; break;              /* MULTI_TOUCH_AVAILABLE = no */
    default: v = 0; break;
    }
    if (prop < 32 && !(seen & (1u << prop))) {
        seen |= 1u << prop;
        printf("  [ptr  ] GetInt(%u) -> %u\n", (unsigned)prop, (unsigned)v);
    }
    cpu->r[0] = v;
}

/* ---- surface: the framebuffer the game blits into --------------------
 * Geometry is declared further up, above the input code that uses it. */
static uint8_t *g_surf;
static int g_presents;

static void hle_surface_ptr(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = SURF_BASE;
}

/* The present point. Counting is enough to keep guest-visible behaviour
 * identical to the reference; putting these pixels on the screen is a
 * separate step, since the console owns the display here. */
/* Write the surface out as a binary PPM: no library, and any viewer opens it.
 * This is the only way to see what the game is actually drawing, since the
 * console owns the screen on device and the host has no display at all. */
/* Host-only. On device this wrote a 460 KB .ppm to the SD card at each of the
 * first four presents -- a differential-harness diagnostic that has no purpose
 * here, and one more large write on a card that is already returning EIO. */
static void surf_dump(int n) {
#ifdef __SWITCH__
    (void)n;
#else
    char path[64];
    FILE *f;
    uint32_t i;
    snprintf(path, sizeof path, "frame%04d.ppm", n);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", SCREEN_W, SCREEN_H);
    for (i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint32_t v = (uint32_t)g_surf[2 * i] | ((uint32_t)g_surf[2 * i + 1] << 8);
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(((v >> 11) & 0x1F) * 255 / 31);
        rgb[1] = (unsigned char)(((v >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (unsigned char)((v & 0x1F) * 255 / 31);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  [surf ] wrote %s\n", path);
#endif
}

/* Hash of the presented pixels, so two runs can be compared without diffing
 * image files -- the question is usually "did anything change at all". */
static uint32_t surf_hash(void) {
    uint32_t h = 2166136261u, i;
    for (i = 0; i < SURF_FRAME; i++) {
        h ^= g_surf[i];
        h *= 16777619u;
    }
    return h;
}

/* One-shot probe: is what we present a COPY of something still sitting in the
 * guest heap? If it is, the game composites into its own buffer and blits, and
 * the source address tells us where to look next. If it is not, the game
 * rasterises straight into the surface and the content itself is the problem.
 * Either answer redirects the search, which is why it is worth one scan. */
static void surf_find_source(int frame) {
    static int done;
    uint32_t heap_used = g_brk - HEAP_BASE;
    uint32_t i, start = 0;
    const uint32_t NEED = 96;
    uint8_t *needle;

    if (done || frame < 300 || !g_surf || !g_heap)
        return;
    done = 1;

    /* Pick a window that is not flat, or we would match anything. */
    for (i = 0; i + NEED <= SURF_FRAME; i += NEED) {
        uint32_t k, distinct = 0;
        for (k = 1; k < NEED; k++)
            if (g_surf[i + k] != g_surf[i])
                distinct++;
        if (distinct > NEED / 2) {
            start = i;
            break;
        }
    }
    needle = g_surf + start;
    printf("  [surf ] searching %u MB of heap for the presented pixels "
           "(surface offset %u)\n", heap_used >> 20, start);

    for (i = 0; i + NEED <= heap_used; i++) {
        uint8_t *p = memchr(g_heap + i, needle[0], heap_used - i - NEED);
        if (!p)
            break;
        i = (uint32_t)(p - g_heap);
        if (memcmp(p, needle, NEED) == 0) {
            printf("  [surf ] FOUND identical pixels at guest %08x "
                   "(heap+%u) -- the surface is a COPY\n",
                   (unsigned)(HEAP_BASE + i), (unsigned)i);
            return;
        }
    }
    printf("  [surf ] no copy in the heap -- the game rasterises directly "
           "into the surface\n");
}

/* Until now this NRO only ever put a text console on screen, so the game's
 * pixels had nowhere to go. The surface is 480x320 RGB565; the panel is
 * 1280x720. 480x320 is 3:2, so a 1080x720 centred box is the exact-aspect fit
 * and the bars either side stay black.
 *
 * The console is kept for the load, which is where every fault so far has
 * happened, and handed over on the first present -- the log goes to `nxlink -s`
 * regardless, so nothing is lost by giving up the screen. */
static Framebuffer g_fb;
static int g_fb_up, g_fb_fail;

/* Exactly one thing may own the default NWindow at a time. consoleInit installs
 * a Framebuffer on it, and so does framebufferCreate, and so does EGL's window
 * surface -- all three go through nwindowSetDimensions/nwindowConfigureBuffer,
 * and the second binder gets LibnxError_AlreadyInitialized. So ownership is
 * explicit and every transition releases before it acquires.
 *
 * Staging: the console keeps the window through load and EGL init (eglGetDisplay
 * and eglInitialize need no window, so the on-TV log survives a failure there),
 * and only eglCreateWindowSurface -- or the first software present -- takes it. */
typedef enum { WIN_NONE, WIN_CONSOLE, WIN_FB, WIN_EGL } WinOwner;
static WinOwner g_win = WIN_CONSOLE;    /* consoleInit(NULL) runs in main() */
extern volatile uint32_t g_native_stage;

static void win_release(void) {
    switch (g_win) {
    case WIN_CONSOLE:
        g_native_stage = 121;
        consoleExit(NULL);
        g_native_stage = 122;
        break;
    case WIN_FB:      framebufferClose(&g_fb);  break;
    case WIN_EGL:     break;   /* only EGL's own teardown releases its buffers */
    default:          break;
    }
    g_win = WIN_NONE;
}

/* Called from the eglSwapBuffers thunk. On the GL path the game never calls
 * s3eSurfaceShow, so g_presents would stay at 0 and the synthetic tap -- armed
 * from inside that handler -- would never fire. The game sits on "TOUCH SCREEN
 * TO START" forever. A GL swap is the same event, so it counts as a frame. */
void egl_frame_presented(void) {
    g_presents++;
    tap_arm(g_presents);
    if (g_presents <= 3 || (g_presents % 100) == 0)
        printf("  [egl  ] frame %d presented via GL\n", g_presents);
}

/* Called from the eglCreateWindowSurface thunk in jit/gl_egl.c. The guest's
 * native-window argument is meaningless here; this is the real one. */
void *egl_take_window(void) {
    void *win;
    g_native_stage = 120;
    win_release();
    g_native_stage = 123;
    g_win = WIN_EGL;
    printf("  [win  ] NWindow handed to EGL\n");
    win = nwindowGetDefault();
    g_native_stage = win ? 130 : 131;
    return win;
}
static uint32_t g_xmap[1080];

static void fb_open(void) {
    Result rc;
    uint32_t x;
    for (x = 0; x < VIEW_W; x++)
        g_xmap[x] = x * SCREEN_W / VIEW_W;
    win_release();
    rc = framebufferCreate(&g_fb, nwindowGetDefault(), FB_W, FB_H,
                           PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) {
        consoleInit(NULL);
        g_win = WIN_CONSOLE;
        printf("  [fb   ] framebufferCreate failed: 0x%08x -- staying on the "
               "console\n", (unsigned)rc);
        g_fb_fail = 1;
        return;
    }
    rc = framebufferMakeLinear(&g_fb);
    if (R_FAILED(rc)) {
        framebufferClose(&g_fb);
        consoleInit(NULL);
        g_win = WIN_CONSOLE;
        printf("  [fb   ] framebufferMakeLinear failed: 0x%08x\n", (unsigned)rc);
        g_fb_fail = 1;
        return;
    }
    g_win = WIN_FB;
    printf("  [fb   ] %ux%u framebuffer up, %ux%u view at x=%u\n",
           FB_W, FB_H, VIEW_W, FB_H, VIEW_X);
    g_fb_up = 1;
}

static void fb_blit(void) {
    uint32_t stride, x, y;
    uint8_t *out = (uint8_t *)framebufferBegin(&g_fb, &stride);
    if (!out)
        return;
    memset(out, 0, (size_t)stride * FB_H);          /* letterbox bars */
    for (y = 0; y < FB_H; y++) {
        const uint8_t *src = g_surf + (size_t)(y * SCREEN_H / FB_H) *
                             SCREEN_W * SURF_BPP;
        uint32_t *dst = (uint32_t *)(out + (size_t)y * stride) + VIEW_X;
        for (x = 0; x < VIEW_W; x++) {
            uint32_t sx = g_xmap[x];
            uint32_t v = (uint32_t)src[2 * sx] | ((uint32_t)src[2 * sx + 1] << 8);
            uint32_t r = (v >> 11) & 0x1F, gg = (v >> 5) & 0x3F, b = v & 0x1F;
            /* bit replication, not *255/31 -- no divides in the inner loop */
            r = (r << 3) | (r >> 2);
            gg = (gg << 2) | (gg >> 4);
            b = (b << 3) | (b >> 2);
            dst[x] = 0xFF000000u | (b << 16) | (gg << 8) | r;
        }
    }
    framebufferEnd(&g_fb);
}

static void fb_close(void) {
    if (g_win == WIN_EGL) {
        /* EGL still holds its buffers; taking the window back would need the
         * display handle, which lives in gl_egl.c. The report goes to nxlink. */
        printf("  [win  ] EGL owns the NWindow; report goes to nxlink only\n");
        return;
    }
    if (!g_fb_up)
        return;
    win_release();
    g_fb_up = 0;
    consoleInit(NULL);                              /* so the report is visible */
    g_win = WIN_CONSOLE;
}

static void hle_surface_show(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    g_presents++;
    /* If EGL is driving the display the game presents with eglSwapBuffers and
     * this surface is not what reaches the screen. */
    if (g_win != WIN_EGL) {
        if (!g_fb_up && !g_fb_fail)
            fb_open();
        if (g_fb_up)
            fb_blit();
    }
    tap_arm(g_presents);
    surf_find_source(g_presents);
    /* Dump densely around the tap so before/after can be compared directly. */
    if (g_presents <= 4 || (g_presents % 100) == 0 ||
        (g_tap_frame > 0 && g_presents >= g_tap_frame - 2 &&
         g_presents <= g_tap_frame + 30)) {
        printf("  [surf ] present #%d hash=%08x\n", g_presents,
               (unsigned)surf_hash());
        surf_dump(g_presents);
    } else if ((g_presents % 25) == 0) {
        printf("  [surf ] present #%d hash=%08x\n", g_presents,
               (unsigned)surf_hash());
    }
    cpu->r[0] = 0;
}

static void hle_surface_setup(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    printf("  [surf ] setup(%08x %08x %08x %08x)\n",
           (unsigned)cpu->r[0], (unsigned)cpu->r[1], (unsigned)cpu->r[2],
           (unsigned)cpu->r[3]);
    cpu->r[0] = 0;
}

/* Property ids as the reference's QUERY table has them; 2 is the pitch.
 *
 * Property 11 is a SCREEN ORIENTATION in quarter turns (0-3), not a flag:
 * RVA 0x0dd46e reads it, then computes `stored_orientation - prop11` and adds
 * 4 if negative -- a rotation delta mod 4. RVA 0x2776ac feeds it straight into
 * a 4-entry jump table (`cmp r0,#3` / `addls pc,pc,r0,lsl #2`). If this
 * disagrees with what the game has stored, it picks a ROTATED blit path and
 * writes turned pixels into the buffer. BOZ_ORIENT sweeps it without a rebuild.
 *
 * Property 6 is queried too (RVA 0x27b87c) and is almost certainly the device
 * pitch; answering 0 for a pitch invites a divide-by-zero or a degenerate
 * blit, so it mirrors the surface pitch.
 * Property 8 is read at 0x0dd47a and immediately discarded at that site. */
static int g_orient = -1;

static void hle_surface_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0], v;
    (void)mem; (void)user;
    if (g_orient < 0) {
        const char *e = getenv("BOZ_ORIENT");
        g_orient = e ? atoi(e) : 0;
    }
    switch (prop) {
    case 0: case 4: v = SCREEN_W; break;
    case 1: case 5: v = SCREEN_H; break;
    case 2: case 6: v = SCREEN_W * SURF_BPP; break;   /* pitch, device pitch */
    case 3:         v = SURF_PIXTYPE; break;
    case 11:        v = (uint32_t)g_orient; break;    /* orientation, 0-3 */
    default:        v = 0; break;
    }
    if (prop < 32 && !(seen & (1u << prop))) {
        seen |= 1u << prop;
        printf("  [surf ] GetInt(%u) -> %u\n", (unsigned)prop, (unsigned)v);
    }
    cpu->r[0] = v;
}

/* s3eGLGetInt(0) is S3E_GL_VERSION. The game compares it against **0x100** at
 * three sites (RVA 0x27bba8, 0x27ffa8, 0x2c96d4), so the encoding is packed
 * major/minor -- 0x100 = 1.0, 0x110 = 1.1, 0x200 = 2.0 -- NOT the 0x11 the
 * Unicorn reference used. 0x11 is 17, far below every one of those tests, so
 * it silently selects the lowest-capability path everywhere. The reference
 * carries the same value, which would explain why it never rendered either.
 * BOZ_GLVER overrides for sweeping. Property 2 is also queried (compared
 * against 0 and 1) and looks like a boolean capability. */
static int g_glver = -1;

static void hle_gl_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0], v;
    (void)mem; (void)user;
    if (g_glver < 0) {
        const char *e = getenv("BOZ_GLVER");
        g_glver = e ? (int)strtol(e, NULL, 0) : 0x110;
    }
    v = (prop == 0) ? (uint32_t)g_glver : 0u;
    if (prop < 32 && !(seen & (1u << prop))) {
        seen |= 1u << prop;
        printf("  [gl   ] GetInt(%u) -> 0x%x\n", (unsigned)prop, (unsigned)v);
    }
    cpu->r[0] = v;
}

/* A virtual clock, 16 ms per query, matching the Unicorn reference exactly.
 * Real time would make the run non-deterministic and destroy the differential;
 * but the stub that returned a constant 0 was worse than either, because any
 * "wait until N ms have elapsed" loop then never terminates -- that is what
 * pinned the run in a spin from 10M to 50M instructions. */
static uint32_t g_ticks;

static void hle_timer_ms(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    g_ticks += 16;
    cpu->r[0] = g_ticks;
}

/* Log each unimplemented import once rather than the first N calls: the
 * interesting question is *which* imports are still stubbed, not how often the
 * early ones fire. Every name printed here is a candidate for the next thing
 * to port from run_boz.py. */
static unsigned g_stub_calls[512];
static uint32_t g_memory_bucket;

/* RVA 0x34a2dc proves property 0 is the current allocation bucket: the wrapper
 * saves GetInt(0), SetInt(0,7), allocates, then restores SetInt(0,saved).
 * Returning HEAP_SIZE here made 335544320 the bucket id thousands of times.
 * Property 3 is the only size query observed and is used as free memory. */
static void hle_memory_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0];
    uint32_t used = g_live_bytes;
    uint32_t avail = HEAP_SIZE - used;
    uint32_t tail = HEAP_BASE + HEAP_SIZE - g_brk;
    uint32_t v;
    (void)mem; (void)user;
    switch (prop) {
    case 0:  v = g_memory_bucket; break;/* current allocation bucket */
    case 1:  v = used;      break;      /* used */
    case 2:  v = avail;     break;      /* free */
    case 3:  v = tail;      break;      /* largest contiguous new block */
    case 4:  v = avail;     break;      /* lowest free ever */
    case 5:  v = g_peak_bytes; break;   /* highest live usage ever */
    default: v = avail;     break;
    }
    if (prop < 32 && !(seen & (1u << prop))) {
        seen |= 1u << prop;
        printf("  [mem  ] GetInt(%u) -> %u (%u MB used of %u MB), lr=%06x\n",
               (unsigned)prop, (unsigned)v, (unsigned)(used >> 20),
               (unsigned)(HEAP_SIZE >> 20),
               (unsigned)(cpu->r[GUEST_LR] - g_img.load_base));
    }
    cpu->r[0] = v;
}

static void hle_memory_setint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static int shown;
    uint32_t prop = cpu->r[0], value = cpu->r[1];
    (void)mem; (void)user;
    if (prop == 0) {
        g_memory_bucket = value;
        if (shown < 8) {
            printf("  [mem  ] bucket -> %u\n", (unsigned)value);
            shown++;
        }
    }
    cpu->r[0] = 0;                    /* S3E_RESULT_SUCCESS */
}

/* ---- audio ------------------------------------------------------------
 *
 * All 20 of the s3eAudio and s3eSound imports fell to hle_default and
 * game then took Single Player -> Continue and died at RVA 0x2710bc doing
 *     ldr r3, [r3]        ; object->vtable
 *     ldr ip, [r3, #0x14] ; vtable[5]      <- r3 was NULL
 *     blx ip
 * i.e. a virtual call on an object that was allocated but never constructed.
 * A sound system told it has no channels and no device builds exactly that:
 * a live object whose subsystem never came up.
 *
 * There is no audio output here yet, so these report a working-but-idle
 * system: the device exists, channels exist, nothing is currently playing.
 * Property ids are logged on first sight rather than guessed at, which is how
 * LowMemoryDevice was found -- the defaults below are deliberately generous
 * because 0 is the answer that has caused every failure of this kind so far.
 */
#define SND_CHANNELS 16

static uint32_t g_snd_next_channel;
static uint8_t  g_snd_playing[SND_CHANNELS];

static void logprop(const char *who, uint32_t prop, uint32_t v,
                    uint32_t *seen) {
    if (prop < 32 && !(*seen & (1u << prop))) {
        *seen |= 1u << prop;
        printf("  [snd  ] %s(%u) -> %u\n", who, (unsigned)prop, (unsigned)v);
    }
}

/* s3eSoundGetInt: 0 is S3E_SOUND_NUM_CHANNELS in every Marmalade build I can
 * check against, and answering 0 there is what leaves the mixer unbuilt. */
static void hle_sound_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0], v;
    (void)mem; (void)user;
    switch (prop) {
    case 0:  v = SND_CHANNELS; break;      /* NUM_CHANNELS  */
    case 1:  v = 1;            break;      /* AVAILABLE     */
    case 2:  v = 44100;        break;      /* OUTPUT_FREQ   */
    case 3:  v = 1;            break;      /* STEREO/other  */
    default: v = 1;            break;
    }
    logprop("SoundGetInt", prop, v, &seen);
    cpu->r[0] = v;
}

static void hle_sound_setint(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = 0;                          /* S3E_RESULT_SUCCESS */
}

static void hle_audio_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0], v;
    (void)mem; (void)user;
    switch (prop) {
    case 0:  v = 1;   break;                /* AVAILABLE  */
    case 1:  v = 0;   break;                /* STATUS: not playing */
    case 2:  v = 256; break;                /* VOLUME (S3E_AUDIO_MAX_VOLUME) */
    default: v = 1;   break;
    }
    logprop("AudioGetInt", prop, v, &seen);
    cpu->r[0] = v;
}

static void hle_audio_setint(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = 0;
}

/* A real free channel, not always 0 -- the game tracks them and would stack
 * every sound onto one slot. -1 is "none free", which we never need to say. */
static void hle_sound_getfreechannel(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t i;
    (void)mem; (void)user;
    for (i = 0; i < SND_CHANNELS; i++) {
        uint32_t ch = (g_snd_next_channel + i) % SND_CHANNELS;
        if (!g_snd_playing[ch]) {
            g_snd_next_channel = (ch + 1) % SND_CHANNELS;
            cpu->r[0] = ch;
            return;
        }
    }
    cpu->r[0] = 0;
}

static void hle_sound_channel_play(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t ch = cpu->r[0];
    (void)mem; (void)user;
    if (ch < SND_CHANNELS)
        g_snd_playing[ch] = 1;
    cpu->r[0] = 0;                          /* S3E_RESULT_SUCCESS */
}

static void hle_sound_channel_stop(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t ch = cpu->r[0];
    (void)mem; (void)user;
    if (ch < SND_CHANNELS)
        g_snd_playing[ch] = 0;
    cpu->r[0] = 0;
}

/* s3eSoundChannelGetInt(channel, prop). Property 0 is STATUS: non-zero means
 * still playing, and a sound that never reports finished can wedge a queue. */
static void hle_sound_channel_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t ch = cpu->r[0], prop = cpu->r[1];
    (void)mem; (void)user;
    if (prop == 0)
        cpu->r[0] = (ch < SND_CHANNELS) ? g_snd_playing[ch] : 0;
    else
        cpu->r[0] = 0;
}

static void hle_audio_ok(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = 0;                          /* S3E_RESULT_SUCCESS */
}

static void hle_default(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t idx = (uint32_t)(uintptr_t)user;
    static unsigned char seen[512];
    (void)mem;
    if (idx < sizeof seen && !seen[idx]) {
        seen[idx] = 1;
        /* Arguments on first sight: for an unimplemented import the question is
         * usually "what was it asked for". Kept identical to the host harness
         * so device and host logs can be diffed line for line. */
        printf("  [hle ] %-28s r0=%08x r1=%08x r2=%08x r3=%08x\n",
               slot_name(idx), (unsigned)cpu->r[0], (unsigned)cpu->r[1],
               (unsigned)cpu->r[2], (unsigned)cpu->r[3]);
        g_shown++;
    }
    if (idx < 512)
        g_stub_calls[idx]++;
    g_calls++;
    cpu->r[0] = 0;
}

/* Ranked stub call counts: the cheapest way to see whether an experiment
 * changed the execution path at all, without diffing traces. */
static void dump_stub_calls(void) {
    unsigned i, k;
    printf("\nstub call counts (top 20):\n");
    for (k = 0; k < 20; k++) {
        unsigned best = 0, bi = 0;
        for (i = 0; i < 512; i++)
            if (g_stub_calls[i] > best) {
                best = g_stub_calls[i];
                bi = i;
            }
        if (!best)
            break;
        printf("  %-34s %u\n", slot_name(bi), best);
        g_stub_calls[bi] = 0;       /* consumed, so the next pass ranks below */
    }
}

/* -------------------------------------------------------------------- run */

static unsigned char *slurp(const char *path, size_t *out) {
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    if (n <= 0 || !(b = malloc((size_t)n))) {
        fclose(f);
        return NULL;
    }
    *out = fread(b, 1, (size_t)n, f);
    fclose(f);
    return b;
}

static GuestHleSlot g_slots[512];
static Guest g;
volatile uint32_t g_native_stage;

/* libnx userland exception capture.  This turns an otherwise opaque Atmosphere
 * process termination into a persistent native+guest crash record. */
u8 __nx_exception_stack[0x10000] __attribute__((aligned(16)));
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    static const char *stage_paths[] = {
        "sdmc:/switch/boz/startup_stage.txt", "sdmc:/startup_stage.txt"
    };
    char summary[192];
    unsigned i;
    {
        const char *hle = "-";
        uint32_t guest_pc = g.cpu.r[GUEST_PC];
        if (guest_is_stub(guest_pc))
            hle = slot_name(guest_stub_index(guest_pc));
        snprintf(summary, sizeof summary,
                 "NATIVE EXCEPTION desc=%x native_pc=%llx far=%llx esr=%x "
                 "guest_n=%llu guest_pc=%08x hle=%s native_stage=%u",
                 (unsigned)ctx->error_desc,
                 (unsigned long long)ctx->pc.x,
                 (unsigned long long)ctx->far.x, (unsigned)ctx->esr,
                 (unsigned long long)g.executed, (unsigned)guest_pc, hle,
                 (unsigned)g_native_stage);
    }

    for (i = 0; i < sizeof(stage_paths) / sizeof(stage_paths[0]); i++) {
        FILE *f = fopen(stage_paths[i], "wb");
        if (!f)
            continue;
        fprintf(f, "%s\n", summary);
        fclose(f);
    }

    {
        FILE *f = fopen("sdmc:/switch/boz/exception_dump.txt", "wb");
        if (!f)
            f = fopen("sdmc:/exception_dump.txt", "wb");
        if (f) {
            fprintf(f, "%s\npstate=%08x afsr0=%08x afsr1=%08x\n",
                    summary, (unsigned)ctx->pstate, (unsigned)ctx->afsr0,
                    (unsigned)ctx->afsr1);
            for (i = 0; i < 29; i++)
                fprintf(f, "x%-2u=%016llx%s", i,
                        (unsigned long long)ctx->cpu_gprs[i].x,
                        (i & 3u) == 3u ? "\n" : " ");
            fprintf(f, "\nfp=%016llx lr=%016llx sp=%016llx\n",
                    (unsigned long long)ctx->fp.x,
                    (unsigned long long)ctx->lr.x,
                    (unsigned long long)ctx->sp.x);
            for (i = 0; i < 16; i++)
                fprintf(f, "r%-2u=%08x%s", i, (unsigned)g.cpu.r[i],
                        (i & 3u) == 3u ? "\n" : " ");
            fprintf(f, "guest_cpsr=%08x it=%02x\nrecent guest pc:",
                    (unsigned)g.cpu.cpsr, (unsigned)g.cpu.itstate);
            for (i = 0; i < 16; i++) {
                uint32_t idx = (g.hist_pos + i) & 15u;
                fprintf(f, " %08x", (unsigned)g.hist[idx]);
            }
            fprintf(f, "\n");
            fclose(f);
        }
    }
}

/* Deliver everything queued. A callback may queue more, so the queue is
 * drained before dispatching rather than during. */
static void cb_pump(void) {
    int n = g_cb_queue_n, i;
    g_cb_queue_n = 0;
    for (i = 0; i < n; i++) {
        Callback *cb = &g_cbs[g_cb_queue[i].slot];
        GuestStatus st;
        if (!cb->used || !cb->fn)
            continue;
        st = guest_call(&g, cb->fn, g_cb_queue[i].sysdata, cb->user);
        if (st != GUEST_OK)
            printf("  [cb   ] %s id=%u stopped: %s\n", cb->kind,
                   (unsigned)cb->id, guest_status_str(st));
    }
}

static void bind_slot(uint32_t i, const char *nm) {
    g_slots[i].name = nm;
    g_slots[i].user = (void *)(uintptr_t)i;
    g_slots[i].fn = hle_default;
    if (!nm)
        return;
#ifdef __SWITCH__
    /* All 247 GL/EGL entry points come from the generated thunk table. Only
     * on device: the host differential harness has no GLES to link against. */
    {
        GuestHleFn glfn = gl_find_thunk(nm);
        if (glfn) {
            g_slots[i].fn = glfn;
            return;
        }
    }
#endif
    if (!strcmp(nm, "s3eDebugTraceLine") || !strcmp(nm, "s3eDebugOutputString") ||
        !strcmp(nm, "s3eDebugPrint") || !strcmp(nm, "s3eDebugErrorShow") ||
        !strcmp(nm, "s3eDebugAssertShow"))
        g_slots[i].fn = hle_trace;
    else if (!strcmp(nm, "s3eMallocBase"))
        g_slots[i].fn = hle_malloc;
    else if (!strcmp(nm, "s3eReallocBase"))
        g_slots[i].fn = hle_realloc;
    else if (!strcmp(nm, "s3eFreeBase"))
        g_slots[i].fn = hle_free;
    else if (!strcmp(nm, "s3eExtGetHash"))
        g_slots[i].fn = hle_extgethash;
    else if (!strcmp(nm, "s3eDeviceGetString"))
        g_slots[i].fn = hle_devstring;
    else if (!strcmp(nm, "s3eDeviceGetInt"))
        g_slots[i].fn = hle_device_getint;
    else if (!strcmp(nm, "s3eConfigGetInt"))
        g_slots[i].fn = hle_configint;
    else if (!strcmp(nm, "s3eConfigGetString"))
        g_slots[i].fn = hle_configstr;
    else if (!strcmp(nm, "s3eSoundGetInt"))
        g_slots[i].fn = hle_sound_getint;
    else if (!strcmp(nm, "s3eSoundSetInt"))
        g_slots[i].fn = hle_sound_setint;
    else if (!strcmp(nm, "s3eSoundGetFreeChannel"))
        g_slots[i].fn = hle_sound_getfreechannel;
    else if (!strcmp(nm, "s3eSoundChannelPlay"))
        g_slots[i].fn = hle_sound_channel_play;
    else if (!strcmp(nm, "s3eSoundChannelStop"))
        g_slots[i].fn = hle_sound_channel_stop;
    else if (!strcmp(nm, "s3eSoundChannelGetInt"))
        g_slots[i].fn = hle_sound_channel_getint;
    else if (!strcmp(nm, "s3eSoundChannelSetInt") ||
             !strcmp(nm, "s3eSoundChannelPause") ||
             !strcmp(nm, "s3eSoundChannelResume"))
        g_slots[i].fn = hle_audio_ok;
    else if (!strcmp(nm, "s3eAudioGetInt"))
        g_slots[i].fn = hle_audio_getint;
    else if (!strcmp(nm, "s3eAudioSetInt"))
        g_slots[i].fn = hle_audio_setint;
    else if (!strcmp(nm, "s3eAudioPlay") ||
             !strcmp(nm, "s3eAudioPlayFromBuffer") ||
             !strcmp(nm, "s3eAudioStop") ||
             !strcmp(nm, "s3eAudioPause") ||
             !strcmp(nm, "s3eAudioResume"))
        g_slots[i].fn = hle_audio_ok;
    else if (!strcmp(nm, "s3eAudioIsPlaying"))
        g_slots[i].fn = hle_audio_setint;   /* 0 = not playing */
    else if (!strcmp(nm, "s3eMemoryGetInt"))
        g_slots[i].fn = hle_memory_getint;
    else if (!strcmp(nm, "s3eMemorySetInt"))
        g_slots[i].fn = hle_memory_setint;
    else if (!strcmp(nm, "s3eTimerGetMs") || !strcmp(nm, "s3eTimerGetUST"))
        g_slots[i].fn = hle_timer_ms;
    else if (!strcmp(nm, "s3eDeviceYield"))
        g_slots[i].fn = hle_device_yield;
    else if (!strcmp(nm, "s3ePointerUpdate"))
        g_slots[i].fn = hle_ptr_update;
    else if (!strcmp(nm, "s3ePointerGetState") ||
             !strcmp(nm, "s3ePointerGetTouchState"))
        g_slots[i].fn = hle_ptr_getstate;
    else if (!strcmp(nm, "s3ePointerGetX") || !strcmp(nm, "s3ePointerGetTouchX"))
        g_slots[i].fn = hle_ptr_getx;
    else if (!strcmp(nm, "s3ePointerGetY") || !strcmp(nm, "s3ePointerGetTouchY"))
        g_slots[i].fn = hle_ptr_gety;
    else if (!strcmp(nm, "s3ePointerGetInt"))
        g_slots[i].fn = hle_ptr_getint;
    else if (ends_with(nm, "UnRegister"))
        g_slots[i].fn = hle_unregister;
    else if (ends_with(nm, "Register"))
        g_slots[i].fn = hle_register;
    else if (!strcmp(nm, "s3eSurfacePtr"))
        g_slots[i].fn = hle_surface_ptr;
    else if (!strcmp(nm, "s3eSurfaceShow"))
        g_slots[i].fn = hle_surface_show;
    else if (!strcmp(nm, "s3eSurfaceSetup"))
        g_slots[i].fn = hle_surface_setup;
    else if (!strcmp(nm, "s3eSurfaceGetInt"))
        g_slots[i].fn = hle_surface_getint;
    else if (!strcmp(nm, "s3eGLGetInt"))
        g_slots[i].fn = hle_gl_getint;
    else if (!strcmp(nm, "s3eFileOpen"))
        g_slots[i].fn = hle_file_open;
    else if (!strcmp(nm, "s3eFileCheckExists"))
        g_slots[i].fn = hle_file_exists;
    else if (!strcmp(nm, "s3eFileRead"))
        g_slots[i].fn = hle_file_read;
    else if (!strcmp(nm, "s3eFileWrite"))
        g_slots[i].fn = hle_file_write;
    else if (!strcmp(nm, "s3eFileSeek"))
        g_slots[i].fn = hle_file_seek;
    else if (!strcmp(nm, "s3eFileTell"))
        g_slots[i].fn = hle_file_tell;
    else if (!strcmp(nm, "s3eFileGetSize") || !strcmp(nm, "s3eFileGetFileInt"))
        g_slots[i].fn = hle_file_size;
    else if (!strcmp(nm, "s3eFileClose"))
        g_slots[i].fn = hle_file_close;
    else if (!strcmp(nm, "s3eFileGetError"))
        g_slots[i].fn = hle_file_error;
    else if (!strcmp(nm, "s3eFileGetChar"))
        g_slots[i].fn = hle_file_getchar;
    else if (!strcmp(nm, "s3eFileDelete"))
        g_slots[i].fn = hle_file_delete;
    else if (!strcmp(nm, "s3eFileMakeDirectory"))
        g_slots[i].fn = hle_file_mkdir;
    else if (!strcmp(nm, "s3eFileFlush"))
        g_slots[i].fn = hle_file_flush;
}

/* Offline crash breadcrumb.  Each stage is committed with fclose(), so an
 * abrupt process termination still leaves the last completed stage on SD.
 * The next launch displays it before doing any guest work. */
static const char *g_stage_paths[] = {
    "sdmc:/switch/boz/startup_stage.txt",
    "sdmc:/startup_stage.txt"
};

static void startup_stage_write(const char *stage) {
    unsigned i;
    for (i = 0; i < sizeof(g_stage_paths) / sizeof(g_stage_paths[0]); i++) {
        FILE *f = fopen(g_stage_paths[i], "wb");
        if (!f)
            continue;
        fwrite(stage, 1, strlen(stage), f);
        fwrite("\n", 1, 1, f);
        fflush(f);
        fclose(f);
    }
    printf("STARTUP STAGE: %s\n", stage);
    /* The staged probes run through the first 5M instructions, and EGL comes
     * up inside that window -- eglGetDisplay and eglInitialize both land under
     * 5M. Once anything takes the NWindow, win_release() has already called
     * consoleExit(), and refreshing a torn-down console from here would be a
     * use-after-free on the way to the very crash this is trying to record. */
    if (g_win == WIN_CONSOLE)
        consoleUpdate(NULL);
}

static int startup_stage_read(char *out, size_t cap) {
    unsigned i;
    for (i = 0; i < sizeof(g_stage_paths) / sizeof(g_stage_paths[0]); i++) {
        FILE *f = fopen(g_stage_paths[i], "rb");
        size_t n;
        if (!f)
            continue;
        n = fread(out, 1, cap - 1, f);
        fclose(f);
        out[n] = 0;
        while (n && (unsigned char)out[n - 1] <= ' ')
            out[--n] = 0;
        if (n)
            return 1;
    }
    return 0;
}

static uint32_t fnv1a32(const unsigned char *p, size_t n) {
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void run(void) {
    static const char *paths[] = {"sdmc:/switch/boz/boz.s3e.unpacked",
                                  "sdmc:/boz.s3e.unpacked"};
    size_t size = 0;
    unsigned char *file = NULL;
    uint32_t n, i;
    GuestStatus st;

    startup_stage_write("01 run entered");

    for (i = 0; i < 2 && !file; i++)
        file = slurp(paths[i], &size);
    if (!file) {
        printf("boz.s3e.unpacked not found on SD\n");
        return;
    }
    {
        uint32_t image_hash = fnv1a32(file, size);
        char image_stage[96];
        snprintf(image_stage, sizeof image_stage,
                 "02 image size=%u fnv=%08x", (unsigned)size,
                 (unsigned)image_hash);
        startup_stage_write(image_stage);
        if (size != 4550559u || image_hash != 0x7BB8168Cu) {
            startup_stage_write("IMAGE MISMATCH - wrong or damaged boz.s3e.unpacked");
            printf("Expected size=4550559 fnv=7bb8168c\n");
            return;
        }
    }
    if (s3e_load(file, size, 0, &g_img) != 0) {
        printf("s3e_load failed: %s\n", s3e_error());
        return;
    }
    startup_stage_write("03 s3e image loaded");

    {   /* the archives and boz_files.idx sit beside the image; the loop above
         * leaves i one past the path that worked */
        static const char *roots[] = {"sdmc:/switch/boz", "sdmc:"};
        const char *root = roots[(i >= 1 && i <= 2) ? i - 1 : 0];
        int nidx = s3e_vfs_init(root);
#ifdef __SWITCH__
        gl_set_allocator(galloc);   /* GL string returns live in guest heap */
#endif
        if (nidx < 0)
            printf("vfs: no boz_files.idx in %s (loose files only)\n", root);
        else
            printf("vfs: %d entries from %s\n", nidx, root);
        s3e_config_set_build_style(s3e_vfs_build_style());
        printf("cfg: ResBuildStyle=%s\n", s3e_config_build_style());
    }
    startup_stage_write("04 VFS and GL allocator ready");

    g_stack = calloc(1, STACK_SIZE);
    g_heap = calloc(1, HEAP_SIZE);
    g_surf = calloc(1, SURF_BYTES);
    if (!g_stack || !g_heap || !g_surf) {
        printf("out of memory\n");
        return;
    }
    startup_stage_write("05 guest buffers allocated");
    guest_mem_add(&g.mem, g_img.load_base, g_img.image_alloc, g_img.image, 1);
    guest_mem_add(&g.mem, STACK_BASE, STACK_SIZE, g_stack, 1);
    guest_mem_add(&g.mem, HEAP_BASE, HEAP_SIZE, g_heap, 1);
    guest_mem_add(&g.mem, SURF_BASE, SURF_BYTES, g_surf, 1);
    startup_stage_write("06 guest memory mapped");

    n = g_img.got_count < 511 ? g_img.got_count : 511;
    for (i = 0; i < n; i++) {
        uint32_t stub = GUEST_STUB_BASE + 4 * i;
        bind_slot(i, slot_name(i));
        memcpy(g_img.image + g_img.got_rva[i], &stub, 4);
    }
    /* one spare slot: the address handed out for extension function tables */
    g_slots[n].name = "<ext stub>";
    g_slots[n].fn = hle_zero;
    g_ext_stub = GUEST_STUB_BASE + 4 * n;
    g.hle.slot = g_slots;
    g.hle.count = n + 1;
    startup_stage_write("07 imports bound");

    static GuestHook hooks[7];
    hooks[0].addr = g_img.load_base + RVA_MGR_MALLOC;
    hooks[0].fn = hook_malloc;
    hooks[1].addr = g_img.load_base + RVA_MGR_REALLOC;
    hooks[1].fn = hook_realloc;
    hooks[2].addr = g_img.load_base + RVA_MGR_FREE;
    hooks[2].fn = hook_free;
    g.hook = hooks;
    hooks[3].addr = g_img.load_base + RVA_IMAGE_HANDLER_READY;
    hooks[3].fn = watch_image_handler;
    hooks[3].observe = 1;
    hooks[4].addr = g_img.load_base + RVA_FAST_ANGLE_NORMALIZE;
    hooks[4].fn = fast_angle_normalize;
    hooks[4].observe = 1;
    hooks[5].addr = g_img.load_base + RVA_NATIVE_MEMCPY;
    hooks[5].fn = hook_native_memcpy;
    hooks[6].addr = g_img.load_base + RVA_NATIVE_MEMSET;
    hooks[6].fn = hook_native_memset;
    g.hook_count = 7;

    g.cpu.r[GUEST_SP] = STACK_BASE + STACK_SIZE - 16;
    g.cpu.cpsr = CPSR_Z;   /* Unicorn's reset state; flags are undefined
                            * at entry, so match the oracle rather than
                            * leave the differential misaligned. */
    g.cpu.r[15] = g_img.entry;        /* RVA 0, ARM mode: CPSR.T stays clear */
    startup_stage_write("08 CPU and hooks ready");

    printf("image %u KB, %u slots\n\n", g_img.image_size >> 10, (unsigned)n);

    /* Run in chunks so the log shows progress live rather than going quiet for
     * the whole budget; guest_run's limit is per call, g.executed accumulates.
     * No instruction cap: the first s3eSurfaceShow is 743.7M instructions in
     * (measured on the host differential), so the 100M this used to stop at
     * could never reach a single frame. It runs until it faults or + is
     * pressed, and the chunk boundary is where the applet gets its turn. */
    st = GUEST_STEP_LIMIT;
    startup_stage_write("09 entering guest interpreter");
    {
        static const uint64_t delta[] = {
            1, 9, 90, 900, 9000, 90000, 900000
        };
        static const char *done[] = {
            "10 guest 1 instruction",
            "11 guest 10 instructions",
            "12 guest 100 instructions",
            "13 guest 1000 instructions",
            "14 guest 10000 instructions",
            "15 guest 100000 instructions",
            "16 guest 1M instructions"
        };
        unsigned probe;
        for (probe = 0; probe < sizeof(delta) / sizeof(delta[0]); probe++) {
            st = guest_run(&g, 0xFFFFFFFFu, delta[probe]);
            if (st != GUEST_STEP_LIMIT)
                break;
            startup_stage_write(done[probe]);
        }
        if (st == GUEST_STEP_LIMIT) {
            for (probe = 0; probe < 40; probe++) {
                char detail[96];
                st = guest_run(&g, 0xFFFFFFFFu, 100000);
                if (st != GUEST_STEP_LIMIT)
                    break;
                snprintf(detail, sizeof detail,
                         "17 guest_n=%llu guest_pc=%08x",
                         (unsigned long long)g.executed,
                         (unsigned)g.cpu.r[GUEST_PC]);
                startup_stage_write(detail);
            }
        }
    }
    while (!g_quit && st == GUEST_STEP_LIMIT) {
        st = guest_run(&g, 0xFFFFFFFFu, 5000000ull);
        if (st != GUEST_STEP_LIMIT)
            break;
        if (!appletMainLoop())
            break;
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) {
            printf("  ... stopped by +\n");
            break;
        }
        printf("  ... %lluM instructions, pc=%06x, %d presents\n",
               (unsigned long long)(g.executed / 1000000ull),
               (unsigned)(g.cpu.r[15] - g_img.load_base), g_presents);
    }
    fb_close();                 /* hand the screen back so the report shows */

    printf("\nstopped: %s\n", guest_status_str(st));
    printf("after %llu instructions, %d import calls\n",
           (unsigned long long)g.executed, g_calls);
    printf("pc=0x%08x %s sp=0x%08x\n", (unsigned)g.cpu.r[15],
           guest_is_thumb(&g.cpu) ? "Thumb" : "ARM", (unsigned)g.cpu.r[GUEST_SP]);
    if (st == GUEST_FAULT_UNDEF)
        printf("undef 0x%08x at 0x%08x\n", (unsigned)g.undef_insn,
               (unsigned)g.undef_pc);
    if (st == GUEST_FAULT_MEM)
        printf("unmapped access at 0x%08x\n", (unsigned)g.fault_addr);
    if (st != GUEST_OK) {
        unsigned k;
        /* The whole file is useful when the fault is inside a fill loop: the
         * working pointer and remaining count are in there somewhere. */
        for (k = 0; k < 16; k++)
            printf(" r%-2u=%08x%s", k, (unsigned)g.cpu.r[k],
                   (k & 3u) == 3u ? "\n" : "");
        printf("cpsr=%08x it=%02x\n", (unsigned)g.cpu.cpsr,
               (unsigned)g.cpu.itstate);
        printf("recent pc:");
        for (k = 0; k < 16; k++) {
            uint32_t idx = (g.hist_pos + k) & 15u;
            printf(" %06x", (unsigned)(g.hist[idx] - g_img.load_base));
        }
        printf("\n");
    }
    dump_stub_calls();
    printf("heap: %u live, %u peak, %u high-water bytes   hooks m/r/f: %d/%d/%d\n",
           (unsigned)g_live_bytes, (unsigned)g_peak_bytes,
           (unsigned)(g_brk - HEAP_BASE), g_hook_hits[0],
           g_hook_hits[1], g_hook_hits[2]);
    printf("jit: %u blocks, %llu / %llu guest instructions compiled (%.1f%%)\n",
           (unsigned)g.jit_blocks, (unsigned long long)g.jit_executed,
           (unsigned long long)g.executed,
           g.executed ? 100.0 * (double)g.jit_executed / (double)g.executed : 0.0);
    printf("fastmem: memcpy %u calls/%llu bytes, memset %u calls/%llu bytes\n",
           (unsigned)g_fast_mem_hits[0],
           (unsigned long long)g_fast_mem_bytes[0],
           (unsigned)g_fast_mem_hits[1],
           (unsigned long long)g_fast_mem_bytes[1]);
}

/* Launched from hbmenu rather than by nxlink, __nxlink_host is zero and
 * nxlinkStdio() has nothing to connect to -- it returns -1 and the whole log
 * is stranded on a console that the framebuffer takes over at the first frame.
 * The address is just a global, so read it off the card instead. Put the PC's
 * IPv4 address in nxlink_host.txt and run a listener on port 28771
 * (NXLINK_CLIENT_PORT); the stream is plain text with no handshake.
 *
 * This removes netloader from the loop entirely, which matters because
 * netloading writes the NRO to the SD, and that write is what has been
 * failing. */
static int nxlink_host_from_file(void) {
    static const char *paths[] = {"sdmc:/switch/boz/nxlink_host.txt",
                                  "sdmc:/nxlink_host.txt"};
    unsigned i;
    for (i = 0; i < 2; i++) {
        char buf[64];
        size_t n;
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        while (n && (unsigned char)buf[n - 1] <= ' ')
            buf[--n] = 0;                       /* trailing newline/space */
        if (inet_pton(AF_INET, buf, &__nxlink_host) == 1) {
            printf("nxlink host %s (from %s)\n", buf, paths[i]);
            return 1;
        }
        printf("bad address \"%s\" in %s\n", buf, paths[i]);
    }
    printf("no nxlink_host.txt on the card\n");
    return 0;
}

int main(int argc, char **argv) {
    int nxfd = -1;
    int sockets_up = 0;
    char previous_stage[128];
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    if (startup_stage_read(previous_stage, sizeof previous_stage)) {
        printf("PREVIOUS RUN LAST REACHED:\n%s\n\nPress A to continue.\n",
               previous_stage);
        consoleUpdate(NULL);
        while (appletMainLoop()) {
            padUpdate(&g_pad);
            if (padGetButtonsDown(&g_pad) & HidNpadButton_A)
                break;
            consoleUpdate(NULL);
            svcSleepThread(10000000ull);
        }
    }

    printf("s3e interpreter test - %s\n\nconnecting to nxlink host...\n",
           BOZ_BUILD_LABEL);
    consoleUpdate(NULL);
    if (R_SUCCEEDED(socketInitializeDefault())) {
        sockets_up = 1;
        nxfd = nxlinkStdio();
        if (nxfd < 0) {                 /* not netloaded: try the card */
            consoleUpdate(NULL);
            if (nxlink_host_from_file())
                nxfd = nxlinkStdio();
        }
        if (nxfd < 0) {
            printf("nxlink connect failed (%d); logging on screen\n", nxfd);
            consoleUpdate(NULL);
        }
    } else {
        printf("socket init failed; logging on screen\n");
        consoleUpdate(NULL);
    }
    setvbuf(stdout, NULL, _IONBF, 0);   /* stream lines as they happen */

    printf("s3e interpreter test - %s\n\n", BOZ_BUILD_LABEL);
    run();
    printf("\nPress + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    if (nxfd >= 0)
        close(nxfd);
    if (sockets_up)
        socketExit();
    consoleExit(NULL);
    return 0;
}
