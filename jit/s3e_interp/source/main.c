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
#include <float.h>
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
#include "boz_build_id.h"   /* generated: hash of the sources in this build */
/* No __DATE__/__TIME__ here on purpose -- they record when THIS file was
 * compiled, which is not when the binary was built, and the difference is
 * exactly what made a working fix look like a failed copy. */
#define BOZ_BUILD_LABEL "jit-r82-" BOZ_BUILD_ID
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
/* On by default. Disabling it was tried in r29 on the theory that the game
 * reads through freed pointers; it fixed neither known fault (r27 died at
 * 3.44B and r29 at 3.5B, same site) and only costs memory, so recycling stays.
 * Put a 0 in sdmc:/switch/boz/recycle.txt to disable it and compare. */
static int g_recycle_freed = 1;

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

/* The allocation containing `addr`, rather than the one starting at it. Only
 * fresh bump allocations are recorded, so g_allocs stays sorted by address and
 * this can binary-search for the last block starting at or below the target.
 * Returns -1 when the address falls in no live block -- which for a field the
 * game is reading means the read is out of bounds. */
static int32_t galloc_containing(uint32_t addr) {
    uint32_t lo = 0, hi = g_alloc_n;
    int32_t best = -1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (g_allocs[mid].addr <= addr) {
            best = (int32_t)mid;
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return best;
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
    /* Do not recycle. The game holds pointers to blocks it has freed -- at
     * least two of them: the image-conversion registry keeps a handler that
     * the string assignment at RVA 0x27e6d8 then reuses and writes 98 times,
     * and the object read at RVA 0x23f228 has the same shape. Handing a freed
     * block straight back out turns each of those into a different object's
     * data appearing where a pointer or vtable is expected.
     *
     * The host oracle has always done exactly this -- gfree is a no-op there,
     * "to preserve oracle address ordering" -- which is the whole reason 2.5B
     * host instructions never reproduced any of it while hardware died every
     * run. Matching that behaviour removes the class rather than the instance.
     *
     * The cost is memory. Recycling held the high-water mark at 27 MB against
     * a 1 GB heap, and the OOM path already reports if that stops being true,
     * so the ceiling is visible rather than silent. */
    if (g_recycle_freed) {
        bucket = (g_allocs[i].size >> 4) & 4095u;
        g_allocs[i].next_free = g_free_head[bucket];
        g_free_head[bucket] = i + 1u;
    }
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
/* Up here because hook_free consults the registry; the handler-repair code
 * below uses the same constant. */
#define RVA_IMAGE_HANDLER_SLOTS 0x49fd88u
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

/* The image-conversion registry holds raw pointers to four handler objects for
 * the life of the process, and the game frees one of them. The allocator then
 * reissues the block: RVA 0x27e6d8 is a string assignment (free the old buffer,
 * NULL it when the source is empty, else malloc 0xa0 and copy), and it wrote
 * 0x600564d0 ninety-eight times while the registry still pointed there. The
 * dispatcher at 0x2710bc then read that string's char* as a vtable -- NULL,
 * or 0x01980118, or 0x009f009e, depending on what the string held.
 *
 * Refusing these four frees leaks at most four small objects. The slots are
 * read fresh rather than cached because an earlier attempt cached them from
 * watch_image_handler, which only runs on the first dispatcher call -- by then
 * the free had already happened, the guard never fired, and it looked like
 * evidence against a use-after-free when it was only evidence of arming late. */
static int is_registry_handler(GuestMem *mem, uint32_t addr) {
    uint32_t slot, i;
    if (!addr || !g_img.load_base)
        return 0;
    for (i = 0; i < 4; i++)
        if (guest_ld32(mem, g_img.load_base + RVA_IMAGE_HANDLER_SLOTS + 4u * i,
                       &slot) && slot == addr)
            return 1;
    return 0;
}

static void hook_free(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    g_hook_hits[2]++;
    if (is_registry_handler(mem, cpu->r[0])) {
        static int refused;
        if (refused < 8) {
            refused++;
            printf("  [img  ] refused free of registered handler %08x\n",
                   (unsigned)cpu->r[0]);
        }
        cpu->r[0] = 0;
        return;
    }
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

/* Software vertex transform, RVA 0x00524c -- 92 bytes, and 6% of every guest
 * instruction the game executes. The top entry in pcprof by a wide margin.
 *
 *   out[i] = ((m[i]*x + m[i+3]*y + m[i+6]*z) >> 12) + m[i+9]   for i = 0,1,2
 *
 * r0 = out (3 x int32), r1 = matrix (12 x int32, Q12), r2 = vertex
 * (3 x int16, sign extended). A 3x3 rotate in 12.12 fixed point plus a
 * translation. The game reports Transform -> HW and then does this on the CPU
 * anyway -- skinning or culling, most likely.
 *
 * Two details decide whether this is bit-exact rather than merely close. MUL
 * and MLA wrap modulo 2^32 and signed overflow is undefined in C, so the
 * accumulation is done in uint32_t and only the shift is signed. And ASR
 * rounds toward minus infinity, not toward zero, so it has to be an arithmetic
 * shift of a signed value rather than a division.
 *
 * The scratch registers are set to what the real function leaves behind: r1
 * from its last load, r2 the sign-extended z, and ip the second output. r4-r8
 * it pushes and pops, so they are untouched here. That makes the hook register
 * identical to the code it replaces, which is what lets the differential
 * harness compare equal instead of reporting three false divergences. */
/* Componentwise 16-bit vector add, RVA 0x00506c -- 26 bytes, 2% of all guest
 * instructions. r0 = out, r1 = a, r2 = b, three u16 components each.
 *
 * The adds are Thumb ADD (register) T2, which does NOT set flags, so there is
 * nothing to reproduce in CPSR. The one observable clobber is r2: the guest
 * computes a.x + b.x into it as a full 32-bit value and only the store
 * truncates, so the register keeps the untruncated sum. */
#define RVA_VEC3_ADD16 0x00506cu

static uint32_t g_vadd_hits, g_vadd_skips;

static void hook_vec3_add16(GuestCpu *cpu, GuestMem *mem, void *user) {
    const void *ap, *bp;
    void *op;
    uint16_t A[3], B[3], O[3];
    (void)user;
    ap = guest_ptr(mem, cpu->r[1], (uint32_t)sizeof A);
    bp = guest_ptr(mem, cpu->r[2], (uint32_t)sizeof B);
    op = guest_wptr(mem, cpu->r[0], (uint32_t)sizeof O);
    if (!ap || !bp || !op) { g_vadd_skips++; return; }
    memcpy(A, ap, sizeof A);
    memcpy(B, bp, sizeof B);
    O[0] = (uint16_t)(A[0] + B[0]);
    O[1] = (uint16_t)(A[1] + B[1]);
    O[2] = (uint16_t)(A[2] + B[2]);
    memcpy(op, O, sizeof O);
    cpu->r[2] = (uint32_t)A[0] + (uint32_t)B[0];
    g_vadd_hits++;
}

/* Normalise, RVA 0x0035d0 -- 2% of all guest instructions in ten bytes:
 *
 *     while ((int32_t)r2 >= 0) { r0--; r2 <<= 1; }   then bx lr
 *
 * The loop head is also the function tail, so a hook here returning to LR is
 * exactly what the guest does. It collapses to one count-leading-zeros.
 *
 * Flags matter here, unlike the vector add. The loop leaves CPSR from its
 * final `cmp r2, #0` on a negative value: N set, Z clear, C set (a subtract of
 * zero never borrows), V clear. The caller is free to branch on that.
 *
 * r2 == 0 is not handled because the guest cannot handle it either -- shifting
 * zero never sets bit 31, so the original spins forever. Counting it and
 * returning is the least-bad answer; a hang would be worse and inventing a
 * result would be a lie. */
#define RVA_NORMALISE 0x0035d0u

static uint32_t g_norm_hits, g_norm_skips;

static void hook_normalise(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t v = cpu->r[2];
    unsigned n;
    (void)mem; (void)user;
    if (!v) { g_norm_skips++; return; }
    n = (unsigned)__builtin_clz(v);
    cpu->r[2] = v << n;
    cpu->r[0] -= n;
    cpu->cpsr = (cpu->cpsr & ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V))
              | CPSR_N | CPSR_C;
    g_norm_hits++;
}

#define RVA_VTX_TRANSFORM 0x00524cu

static uint32_t g_vtx_hits, g_vtx_skips;

static void hook_vtx_transform(GuestCpu *cpu, GuestMem *mem, void *user) {
    const void *mp, *vp;
    void *op;
    int32_t M[12], O[3];
    int16_t V[3];
    int i;
    (void)user;
    mp = guest_ptr(mem, cpu->r[1], (uint32_t)sizeof M);
    vp = guest_ptr(mem, cpu->r[2], (uint32_t)sizeof V);
    op = guest_wptr(mem, cpu->r[0], (uint32_t)sizeof O);
    if (!mp || !vp || !op) {
        /* Unmapped: leave the state alone rather than inventing a result. The
         * real function would have faulted here; this at least does not lie. */
        g_vtx_skips++;
        return;
    }
    memcpy(M, mp, sizeof M);
    memcpy(V, vp, sizeof V);
    for (i = 0; i < 3; i++) {
        uint32_t acc = (uint32_t)M[i]     * (uint32_t)(int32_t)V[0]
                     + (uint32_t)M[i + 3] * (uint32_t)(int32_t)V[1]
                     + (uint32_t)M[i + 6] * (uint32_t)(int32_t)V[2];
        O[i] = (int32_t)((uint32_t)((int32_t)acc >> 12) + (uint32_t)M[i + 9]);
    }
    memcpy(op, O, sizeof O);
    cpu->r[1] = (uint32_t)M[9];
    cpu->r[2] = (uint32_t)(int32_t)V[2];
    cpu->r[12] = (uint32_t)O[1];
    g_vtx_hits++;
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

/* --------------------------------------------------- float guard, natively
 *
 * 96 bytes holding 7% of all guest instructions in a menu, which is what made
 * it worth reading:
 *
 *   61128  vmov s15,r1 / vldr s14,<-FLT_MAX> / vcmpe / vmrs / blt <zero>
 *          vldr s14,<+FLT_MAX> / vcmpe / vldr s14,<0.0> / vmrs
 *          it hi / vmovhi.f32 s15,s14 / vstr s15,[r0]
 *   61168  vmov s14,r1 / vldr s15,[r0] / vmul.f32 / bl 61128
 *
 * So: *r0 = v, with anything not finite replaced by +0.0, and a wrapper that
 * multiplies first. An engine sanitising every float it stores.
 *
 * The range test is written negated so NaN lands where the guest puts it. NaN
 * compares unordered against everything, which leaves N=0 V=1, so the guest's
 * `blt` is taken and NaN becomes zero -- a plain `v < -FLT_MAX || v > FLT_MAX`
 * would read as if NaN fell through to the store instead.
 *
 * In-range values are passed through as the original bit pattern rather than
 * as a float that happens to compare equal, so -0.0 and denormals survive
 * exactly. The multiply uses a plain C float, matching what the interpreter's
 * own VFP path does -- there is no FPSCR emulation here, so this introduces no
 * new divergence. */
#define RVA_F32_GUARD     0x61128u
#define RVA_F32_MUL_GUARD 0x61168u

static uint32_t g_f32_hits[2];

static uint32_t f32_guard(uint32_t bits) {
    float v;
    memcpy(&v, &bits, sizeof v);
    if (!(v >= -FLT_MAX && v <= FLT_MAX))
        return 0;
    return bits;
}

static void hook_f32_guard(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    if (!guest_st32(mem, cpu->r[0], f32_guard(cpu->r[1])))
        printf("  [fast ] f32 guard: store faulted at %08x\n",
               (unsigned)cpu->r[0]);
    g_f32_hits[0]++;
}

static void hook_f32_mul_guard(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t cur, out;
    float a, b;
    (void)user;
    if (!guest_ld32(mem, cpu->r[0], &cur)) {
        printf("  [fast ] f32 guard: load faulted at %08x\n",
               (unsigned)cpu->r[0]);
        return;
    }
    memcpy(&a, &cur, sizeof a);
    memcpy(&b, &cpu->r[1], sizeof b);
    a = a * b;
    memcpy(&out, &a, sizeof out);
    guest_st32(mem, cpu->r[0], f32_guard(out));
    g_f32_hits[1]++;
}

/* ------------------------------------------------ integer divide, natively
 *
 * This image has no hardware divide, so every division is a call into a
 * clz-driven shift-and-subtract routine that computes one quotient bit per
 * three instructions. It is 4-5% of guest instructions in EVERY profile window
 * -- menus, loading and gameplay alike -- which is what makes it worth more
 * than its headline share: nothing else is uniformly hot.
 *
 * Three entry points, read off the disassembly:
 *
 *   378d48  __aeabi_uidiv     subs r2,r1,#1 / bxeq lr / bcc <div0> / ...
 *                             returns quotient in r0, r1 untouched
 *   378f34  __aeabi_uidivmod  cmp r1,#0 / beq <div0> / push {r0,r1,lr}
 *                             bl 378d48 / mul r3,r2,r0 / sub r1,r1,r3
 *                             returns quotient r0, remainder r1
 *   378f54  __aeabi_idiv      cmp r1,#0 / beq <div0> / eor ip,r0,r1 / ...
 *                             signed, truncating toward zero
 *
 * Each hook is placed AFTER that routine's divide-by-zero test, not at its
 * entry. That is deliberate: a replacing hook always returns through LR, so it
 * cannot reproduce the zero case, which branches to __aeabi_idiv0 rather than
 * returning. Hooking past the test leaves the zero path running the game's own
 * code, exactly as before, and guarantees a non-zero divisor here. The skipped
 * instructions are a compare and a branch; the loop they guard is the cost.
 *
 * Nothing is pushed before these offsets, so LR still holds the caller's
 * return address, and r2/r3/ip are caller-saved -- leaving them untouched
 * where the real routine would clobber them cannot be observed. */
#define RVA_UIDIV    0x378d54u    /* past `bxeq lr` / `bcc div0`: r1 >= 2 */
#define RVA_UIDIVMOD 0x378f3cu    /* past `beq div0`: r1 != 0 */
#define RVA_IDIV     0x378f5cu    /* past `beq div0`: r1 != 0 */

static uint32_t g_div_hits[3];

static void hook_uidiv(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = cpu->r[0] / cpu->r[1];
    g_div_hits[0]++;
}

static void hook_uidivmod(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t n = cpu->r[0], d = cpu->r[1], q = n / d;
    (void)mem; (void)user;
    cpu->r[0] = q;
    cpu->r[1] = n - q * d;
    g_div_hits[1]++;
}

static void hook_idiv(GuestCpu *cpu, GuestMem *mem, void *user) {
    int32_t n = (int32_t)cpu->r[0], d = (int32_t)cpu->r[1];
    (void)mem; (void)user;
    /* INT_MIN / -1 is undefined in C and would trap, but the guest routine
     * defines it: it negates the magnitude and wraps, yielding 0x80000000.
     * Match that rather than let the host decide. */
    if (n == (int32_t)0x80000000 && d == -1)
        cpu->r[0] = 0x80000000u;
    else
        cpu->r[0] = (uint32_t)(n / d);
    g_div_hits[2]++;
}

/* ------------------------------------------------------- ctype, natively
 *
 * The 16-byte profile put two ARM leaf functions in the library range among
 * the busiest code in the menus. They are tolower and toupper:
 *
 *   ldr r3,[pc,#36] / ldr r2,[pc,#36] / add r3,pc,r3 / ldr r3,[r3,r2]
 *   ldr r3,[r3] / add r3,r3,r0 / ldrb r3,[r3,#1] / and r3,r3,#3
 *   cmp r3,#1 / addeq r0,r0,#32 / bx lr          <- tolower  (#2/sub = toupper)
 *
 * Ten interpreted ARM instructions, called per character, is what a
 * case-insensitive string compare costs -- and the game keys its resource maps
 * on strings, which is also why an std::map node walk sits nearby in the same
 * profile.
 *
 * The replacement performs the identical table lookup rather than assuming C
 * locale: same __ctype_ptr__ table, same class bits, same 32-bit wraparound on
 * table+c. Anything else would be a behaviour change hiding inside an
 * optimisation, and the Unicorn differential would be right to call it.
 *
 * The GOT slot address is not hardcoded. It is recomputed at install time from
 * the two literals the guest itself loads, so if this image is not laid out the
 * way it was read here the hooks simply do not install. */
#define RVA_TOLOWER 0x36f16cu
#define RVA_TOUPPER 0x36f1a0u
#define RVA_CTYPE_LIT_PC 0x36f17cu    /* pc at `add r3,pc,r3`, i.e. insn+8 */
#define RVA_CTYPE_LIT_A  0x36f198u
#define RVA_CTYPE_LIT_B  0x36f19cu

static uint32_t g_ctype_got;          /* guest address of the __ctype_ptr__ slot */
static uint32_t g_ctype_hits[2];

/* Who calls tolower. 11,284 calls a frame in a menu is not tolower's fault --
 * it is one caller doing case-insensitive work far too often, and replacing
 * that caller removes the loop around the conversion as well as the
 * conversions. LR at hook entry is the return address, so a census of it names
 * the caller directly. Small open-addressed table; a caller that cannot get a
 * slot is simply not counted, which is fine for finding the top one. */
#define CTYPE_CALLERS 64
static uint32_t g_ctype_lr[CTYPE_CALLERS];
static uint32_t g_ctype_lr_n[CTYPE_CALLERS];

static void ctype_note_caller(uint32_t lr) {
    uint32_t h = (lr >> 2) & (CTYPE_CALLERS - 1), i;
    for (i = 0; i < CTYPE_CALLERS; i++) {
        uint32_t k = (h + i) & (CTYPE_CALLERS - 1);
        if (g_ctype_lr[k] == lr) { g_ctype_lr_n[k]++; return; }
        if (!g_ctype_lr[k]) { g_ctype_lr[k] = lr; g_ctype_lr_n[k] = 1; return; }
    }
}

static void ctype_report_callers(uint32_t base) {
    int k, b;
    for (b = 0; b < 4; b++) {
        uint32_t bestv = 0;
        int best = -1;
        for (k = 0; k < CTYPE_CALLERS; k++)
            if (g_ctype_lr_n[k] > bestv) { bestv = g_ctype_lr_n[k]; best = k; }
        if (best < 0 || !bestv)
            break;
        printf("  [fast ]   called from %06x  %u\n",
               (unsigned)(g_ctype_lr[best] - base), (unsigned)bestv);
        g_ctype_lr_n[best] = 0;
    }
    memset(g_ctype_lr, 0, sizeof g_ctype_lr);
    memset(g_ctype_lr_n, 0, sizeof g_ctype_lr_n);
}

/* Resolve the table slot the way the guest does. Returns 0 -- and so disables
 * the hooks -- if anything about the sequence fails to read. */
static uint32_t ctype_resolve_got(GuestMem *mem, uint32_t base) {
    uint32_t a, b;
    if (!guest_ld32(mem, base + RVA_CTYPE_LIT_A, &a) ||
        !guest_ld32(mem, base + RVA_CTYPE_LIT_B, &b))
        return 0;
    return base + RVA_CTYPE_LIT_PC + a + b;
}

static void ctype_convert(GuestCpu *cpu, GuestMem *mem, uint32_t want,
                          int32_t delta, int which) {
    uint32_t p, tbl, cls, c = cpu->r[0];
    if (guest_ld32(mem, g_ctype_got, &p) && guest_ld32(mem, p, &tbl) &&
        guest_ld8(mem, tbl + c + 1u, &cls) && (cls & 3u) == want)
        cpu->r[0] = c + (uint32_t)delta;
    /* Otherwise r0 is already the answer: a character of any other class is
     * returned unchanged, which is also the safe result if the table read
     * fails on an out-of-range argument. */
    g_ctype_hits[which]++;
    ctype_note_caller(cpu->r[GUEST_LR]);
}

static void hook_tolower(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    ctype_convert(cpu, mem, 1u, 32, 0);
}

static void hook_toupper(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    ctype_convert(cpu, mem, 2u, -32, 1);
}

/* r25 died at RVA 0x23f234 dereferencing 0x033d0080 -- a heap pointer with its
 * top nibble cleared, since 0x633d0080 & 0x0FFFFFFF is exactly that, and r11
 * held a healthy 0x633d90f4 nearby. Z was clear at the fault, so the preceding
 * `addeq` did not run and the value came from `ldr r3,[r3,#0x84]` two
 * instructions earlier: it was already wrong in memory.
 *
 * This observe hook fires just before that load and names the guest word the
 * bad pointer lives in, which is the address to watch next. It only speaks
 * when the value actually looks truncated, so it stays silent otherwise.
 *
 * It has to run on hardware: hostrun reached 2.5B instructions with 97 imports
 * against the device's 5688 at the same point, so the host never gets here. */
#define RVA_IMG_PTR_LOAD 0x23f228u

/* The struct-copy loop that writes the field. r31 established the field is
 * inside the object's own 288-byte block, so this is not an out-of-bounds
 * read -- one subsystem fills the block with 20-byte vertex structs while
 * another reads +0x84 as a pointer.
 *
 * The remaining question is whether the loop is writing inside its own buffer
 * (two owners for one block, as with the image handler) or has walked past the
 * end of a different buffer into this one. Comparing the block containing the
 * loop's cursor against the block containing the object answers it. Reported
 * only when the cursor actually lands on the watched word, so the hot loop
 * pays a compare and nothing else. */
#define RVA_STRUCT_COPY_STORE 0x2431ecu

/* object = *(*(GOT + 0x17f4)); the GOT slot at RVA 0x412808 holds 0x4a492f74,
 * so this BSS global is the one storing the object pointer. Same address every
 * run, which is what makes it watchable from startup. */
#define RVA_OBJ_GLOBAL 0x492f74u

static void watch_struct_copy(GuestCpu *cpu, GuestMem *mem, void *user) {
    static int logged;
    uint32_t dst = cpu->r[3] - 2u;
    int32_t bi;
    (void)user;
    /* Any store landing in the watched word, matching the store watch's own
     * 4-byte window. This is a halfword store and it lands on watch_addr+2 --
     * testing for an exact match rejected the one write it exists to catch. */
    if (logged >= 6 || !mem->watch_addr || (dst - mem->watch_addr) >= 4u)
        return;
    logged++;
    bi = galloc_containing(cpu->r[3]);
    if (bi >= 0)
        printf("  [copy ] cursor %08x is in block %08x+%u (offset %u)\n",
               (unsigned)cpu->r[3], (unsigned)g_allocs[bi].addr,
               (unsigned)g_allocs[bi].size,
               (unsigned)(cpu->r[3] - g_allocs[bi].addr));
    else
        printf("  [copy ] cursor %08x is in no recorded block\n",
               (unsigned)cpu->r[3]);
}

/* The watch is armed here now, not on the image handler: that fault is handled
 * by the vtable repair, and this is the site still killing every run.
 *
 * The field is not a corrupted pointer. Across four runs it has held
 * 033d0080, 02f20080, 06210080 and 06da0080 -- low halfword always 0x0080,
 * high halfword varying -- so it is a two-u16 structure being read where the
 * game expects a pointer or NULL. r29 ran with allocator recycling disabled,
 * which makes fresh blocks strictly contiguous and zero-filled, and the value
 * appeared anyway. Nothing aliased it; something wrote it. With allocations
 * contiguous, an overflow out of the neighbouring block is the likeliest
 * writer, and the watch will name it. */
static void watch_truncated_ptr(GuestCpu *cpu, GuestMem *mem, void *user) {
    static int logged;
    uint32_t src = cpu->r[3] + 0x84u, val = 0;
    (void)user;
    if (!guest_ld32(mem, src, &val))
        return;

    /* No longer arms the watch. r33 settled what this fault is: the copy
     * loop's cursor and the object live in the *same* 288-byte block (cursor
     * at offset 136, the read field at 132), so nothing is out of bounds --
     * one subsystem fills the block with 20-byte vertex structs while this
     * code reads offset 132 as a pointer. The block is not the object this
     * code wants.
     *
     * The object comes from *(*(GOT+0x17f4)), which resolves statically to the
     * BSS global at RVA 0x492f74 -- a fixed address in every run, unlike the
     * heap addresses. RVA_OBJ_GLOBAL is watched from startup instead, so the
     * writer that stores a vertex block into that global names itself. */

    if (logged < 12 && val >= 0x01000000u && val < 0x10000000u) {
        uint32_t g = 0;
        int32_t bi;
        guest_ld32(mem, g_img.load_base + RVA_OBJ_GLOBAL, &g);
        printf("  [glob ] global %08x -> obj %08x | %llu writes, "
               "last from RVA %06x\n",
               (unsigned)(g_img.load_base + RVA_OBJ_GLOBAL), (unsigned)g,
               (unsigned long long)mem->watch_changes,
               (unsigned)(mem->watch_last_pc - g_img.load_base));
        bi = galloc_containing(cpu->r[3]);
        if (bi >= 0) {
            uint32_t base = g_allocs[bi].addr, size = g_allocs[bi].size;
            uint32_t off = cpu->r[3] - base;
            printf("  [trunc] obj %08x lives in block %08x+%u (offset %u); "
                   "field +0x84 is %s\n", (unsigned)cpu->r[3], (unsigned)base,
                   (unsigned)size, (unsigned)off,
                   (off + 0x88u <= size) ? "INSIDE" : "PAST THE END");
        } else {
            printf("  [trunc] obj %08x is in no recorded block\n",
                   (unsigned)cpu->r[3]);
        }
    }
    if (logged < 12 && val >= 0x01000000u && val < 0x10000000u) {
        logged++;
        printf("  [trunc] %08x at %08x obj=%08x lr=%06x | watch %08x: "
               "%llu writes, last from RVA %06x\n",
               (unsigned)val, (unsigned)src, (unsigned)cpu->r[3],
               (unsigned)(cpu->r[GUEST_LR] - g_img.load_base),
               (unsigned)mem->watch_addr,
               (unsigned long long)mem->watch_changes,
               (unsigned)(mem->watch_last_pc - g_img.load_base));
    }
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

/* r24 ruled out a use-after-free: the free guard tried here never fired once,
 * in a run that reached the fault. These objects are never freed -- their
 * vtable is zeroed in place while the registry still points at them, which is
 * what the repair below exists to undo. */

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

    /* The break matters: `registered` is the repair target further down, so
     * running the loop to completion would leave it holding slot 3's pointer
     * and repair the wrong object. r24 did exactly that -- it restored
     * 600564e0 while 600564d0, the object that faulted, kept vtable 0. */
    for (i = 0; i < 4; i++) {
        if (guest_ld32(mem, g_img.load_base + RVA_IMAGE_HANDLER_SLOTS +
                      4u * (uint32_t)i, &registered) &&
            registered == cpu->r[0]) {
            slot = i;
            break;
        }
    }
    if (!guest_ld32(mem, cpu->r[0], &vtable))
        return;

    /* r23 settled what r20 and r22 could only hint at. Both died at RVA
     * 0x2710bc reading vtable[5] of object 0x600564d0, whose first word held
     * 0x01980118 -- and early in r23 that same object is entirely healthy:
     * vtable 0x4a409f78, which is exactly vtable_rva[2], slot 2, format 0x27.
     * So the object is constructed correctly and then overwritten. 0x01980118
     * is not a memset pattern; as little-endian halfwords it is 280 and 408,
     * i.e. some other object's fields. With 322k frees in a session the
     * mechanism is a use-after-free: the game frees a handler, the allocator
     * reissues the block, and the registry keeps the stale pointer.
     *
     * So complain rather than repair. Writing the vtable back would scribble
     * on whatever owns the block now; hook_free refuses the free instead. */
    /* Arm the store watch on the object itself the first time it is seen
     * healthy. This address has been 600564d0 in every run since r20, and it
     * is the object that gets clobbered -- with 00000000, then 01980118, then
     * 009f009e, i.e. different garbage each time and again after a repair. So
     * this is not a zeroing memset, not a use-after-free and not truncation:
     * something writes over a live object, repeatedly. The watch names it. */
    /* No longer arms the watch: this fault is handled by the repair below, and
     * the single watch slot is needed for the RVA 0x23f228 site, which still
     * kills every run. Whichever site armed first would starve the other. */
    if (slot >= 0 && vtable != g_img.load_base + vtable_rva[slot]) {
        static int warned;
        if (warned < 8) {
            warned++;
            printf("  [trunc] watch %08x: %llu writes, last from RVA %06x\n",
                   (unsigned)mem->watch_addr,
                   (unsigned long long)mem->watch_changes,
                   (unsigned)(mem->watch_last_pc - g_img.load_base));
            printf("  [img  ] slot%d object %08x has vtable %08x, expected "
                   "%08x (fmt 0x%x, lr %06x)\n", slot, (unsigned)cpu->r[0],
                   (unsigned)vtable,
                   (unsigned)(g_img.load_base + vtable_rva[slot]),
                   (unsigned)format,
                   (unsigned)(cpu->r[GUEST_LR] - g_img.load_base));
        }
    }
    if (slot < 0 || vtable != 0)
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

/* An exact match including the handler, so re-registering the same function is
 * idempotent while a second, different handler for the same event is kept.
 * There is deliberately no find-by-event-alone any more: every previous use of
 * it picked an arbitrary listener out of several, which is how registrations
 * got overwritten and unregistrations unhooked the wrong subsystem. */
static int cb_find_fn(const char *kind, uint32_t id, uint32_t fn) {
    int i;
    for (i = 0; i < g_cb_n; i++)
        if (g_cbs[i].used && g_cbs[i].id == id && g_cbs[i].fn == fn &&
            !strcmp(g_cbs[i].kind, kind))
            return i;
    return -1;
}

static void hle_register(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t idx = (uint32_t)(uintptr_t)user;
    char kind[28];
    int i;
    (void)mem;
    cb_kind(slot_name(idx), kind, sizeof kind);
    /* Registrations accumulate. Only an identical (kind, id, fn) is treated as
     * a repeat -- re-registering the same handler must not stack it up -- but a
     * different function for the same event is an additional listener, not a
     * replacement. Overwriting here is what silently unhooked the engine's
     * pointer handler when the UI layer registered its own. */
    i = cb_find_fn(kind, cpu->r[0], cpu->r[1]);
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
    /* Only ever remove the exact handler named. Falling back to "the first
     * registration for this event" is how input dies mid-session: the game
     * tears down a UI listener and we unhook the engine's instead, after which
     * events are still generated and delivered to nothing. */
    i = cb_find_fn(kind, cpu->r[0], cpu->r[1]);
    if (i >= 0) {
        g_cbs[i].used = 0;
        printf("  [cb   ] -%-17s id=%-3u fn=%08x\n", kind,
               (unsigned)cpu->r[0], (unsigned)cpu->r[1]);
    } else {
        printf("  [cb   ] -%-17s id=%-3u fn=%08x NOT FOUND, keeping all\n",
               kind, (unsigned)cpu->r[0], (unsigned)cpu->r[1]);
    }
    cpu->r[0] = 0;
}

/* Queue one for the next yield rather than calling straight away: a callback
 * fired from the middle of an unrelated import would re-enter the guest at a
 * point it does not expect. */
/* Queue every callback registered for the event, not just one. s3eRegister
 * appends -- the game registers a pointer handler in the engine and another in
 * the UI layer, and both are meant to run. Delivering only one is why the
 * menus responded (the UI handler, registered second) while gameplay did not
 * (the engine handler, evicted by it). */
static int cb_queue(const char *kind, uint32_t id, uint32_t sysdata) {
    int i, queued = 0;
    for (i = 0; i < g_cb_n; i++) {
        if (!g_cbs[i].used || g_cbs[i].id != id || strcmp(g_cbs[i].kind, kind))
            continue;
        if (g_cb_queue_n >= (int)(sizeof g_cb_queue / sizeof *g_cb_queue))
            break;
        g_cb_queue[g_cb_queue_n].slot = i;
        g_cb_queue[g_cb_queue_n].sysdata = sysdata;
        g_cb_queue_n++;
        queued++;
    }
    return queued;
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
    {   /* A change in listener count is the thing to catch: input that worked
         * and then stopped means handlers went away while events kept flowing. */
        static int last_n = -1;
        int n = cb_queue("s3ePointer", 0, *buf);
        if (n != last_n) {
            last_n = n;
            printf("  [tap  ] s3ePointer id=0 now has %d listener%s\n", n,
                   n == 1 ? "" : "s");
        }
    }
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

/* ---- multi-touch ------------------------------------------------------
 *
 * This is a dual-virtual-stick shooter: move with the left thumb, look and
 * fire with the right, at the same time. Reporting
 * S3E_POINTER_MULTI_TOUCH_AVAILABLE as 0 made the game register only the
 * single-touch callbacks (s3ePointer id 0 and 1, which is exactly what the
 * callback log shows) and left it with no usable in-game control scheme --
 * menus worked because a menu never needs a second finger.
 *
 * Both paths are driven, as a real device does: the primary contact still
 * produces button and motion events so menus keep working, and every contact
 * additionally produces touch events.
 *
 *   id 2  S3E_POINTER_TOUCH_EVENT        { x, y, touchID, pressed }
 *   id 3  S3E_POINTER_TOUCH_MOTION_EVENT { x, y, touchID }
 */
#define MAX_TOUCH 4

typedef struct {
    int      active;
    int      x, y;
    int      raw_x, raw_y;           /* panel coordinate, kept for the log */
    uint32_t fid;                    /* Switch finger id, to track identity */
} Touch;

static Touch g_touch[MAX_TOUCH];
static uint32_t g_ev_touch[MAX_TOUCH], g_ev_touch_motion[MAX_TOUCH];

/* One event buffer per slot. cb_queue stores the guest pointer and the queue is
 * not drained until s3eDeviceYield, so a single shared buffer lets a second
 * finger's coordinates overwrite the first's before either is delivered --
 * exactly the case multi-touch exists to support. */
static void touch_fire(GuestMem *mem, int slot, int pressed) {
    uint32_t *buf = &g_ev_touch[slot];
    int n;
    if (!*buf)
        *buf = galloc(16);
    if (!*buf)
        return;
    /* { m_TouchID, m_Pressed, m_x, m_y } -- read off the game's own handler at
     * RVA 0xdda20, which does `ldm r1,{r0,r1,r2,r3}`, tests word[1] with cbz
     * as a boolean, uses word[0] to index its table of live touches, and
     * passes &word[2] and &word[3] to the coordinate routine. Same shape as
     * s3ePointerEvent {Button, Pressed, x, y}, which tap_fire already used.
     * Putting x and y first made the game read touch ID 231 at (0,1). */
    guest_st32(mem, *buf + 0, (uint32_t)slot);
    guest_st32(mem, *buf + 4, (uint32_t)pressed);
    guest_st32(mem, *buf + 8, (uint32_t)g_touch[slot].x);
    guest_st32(mem, *buf + 12, (uint32_t)g_touch[slot].y);
    n = cb_queue("s3ePointer", 2, *buf);
    /* Uncapped, and carrying the panel coordinate that produced it: correlating
     * "I touched here and the game did that" needs both halves on every event,
     * and a per-session cap silently stops answering exactly when a long play
     * session starts producing the interesting cases. Volume is one line per
     * press or release, not per frame. */
    printf("  [touch] id=%d %s panel (%d,%d) -> guest (%d,%d) -> %d listener%s\n",
           slot, pressed ? "DOWN" : "up  ", g_touch[slot].raw_x,
           g_touch[slot].raw_y, g_touch[slot].x, g_touch[slot].y, n,
           n == 1 ? "" : "s");
}

static void touch_motion_fire(GuestMem *mem, int slot) {
    uint32_t *buf = &g_ev_touch_motion[slot];
    if (!*buf)
        *buf = galloc(12);
    if (!*buf)
        return;
    /* { m_TouchID, m_x, m_y } -- the handler at RVA 0xddb2c does
     * `ldm r1,{r0,r1,r2}` and passes words 1 and 2 to the same coordinate
     * routine the touch event uses, leaving word[0] as the id. */
    guest_st32(mem, *buf + 0, (uint32_t)slot);
    guest_st32(mem, *buf + 4, (uint32_t)g_touch[slot].x);
    guest_st32(mem, *buf + 8, (uint32_t)g_touch[slot].y);
    cb_queue("s3ePointer", 3, *buf);
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
/* Touch Y origin. The panel reports top-left, but the guest viewport goes to
 * glViewport unflipped and GL's origin is bottom-left, so guest y=0 is the
 * BOTTOM of the screen. Confirmed on hardware: a press near panel y=690 -- the
 * bottom edge, on the Back button -- mapped to guest y=308 and activated
 * something at the top of the screen instead.
 *
 * The rendering is self-consistent either way, which is why this could not be
 * read off a screenshot. Put 0 in sdmc:/switch/boz/flipy.txt to go back. */
/* Bitmask, because the two delivery paths may not agree:
 *   bit 0 (1) - single-touch: s3ePointer id 0/1 and the polling getters,
 *               which the game's UI layer handles
 *   bit 1 (2) - multi-touch: s3ePointer id 2/3, which the engine handles
 * "Sometimes the right spot, sometimes the opposite" is what a disagreement
 * between them looks like, and menus worked before the flip while gameplay
 * needed it. Put 0-3 in flipy.txt; 3 flips both, 2 flips only the engine. */
/* Default 1: UI layer flipped, engine not.
 *
 * Both halves are observed, not reasoned. In the menus, Back at the bottom of
 * the screen activated something at the top when unflipped -- the UI handler
 * (s3ePointer id 0/1) needs the flip. In game, with the flip on, the knife
 * drawn at the bottom responded to a touch at the top, pause drawn top-left
 * responded at the bottom, and character movement was inverted vertically but
 * correct horizontally -- the engine handler (id 2/3) needs no flip. That last
 * detail is decisive: the virtual stick works on deltas, so a wrong Y inverts
 * one axis and leaves the other alone, which is exactly what it did. */
static int g_flip_y = 1;
#define FLIP_SINGLE (g_flip_y & 1)
#define FLIP_MULTI  (g_flip_y & 2)
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
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0 &&
        (int)ts.touches[0].x >= (int)VIEW_X &&
        (int)ts.touches[0].x < (int)(VIEW_X + VIEW_W)) {
        *px = clampi(((int)ts.touches[0].x - (int)VIEW_X) * (int)SCREEN_W
                     / (int)VIEW_W, 0, (int)SCREEN_W - 1);
        *py = clampi((int)ts.touches[0].y * (int)SCREEN_H / (int)FB_H,
                     0, (int)SCREEN_H - 1);
        if (FLIP_SINGLE)
            *py = (int)SCREEN_H - 1 - *py;
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
            /* Guest y grows upward when g_flip_y is set, so pushing the stick
             * up has to increase it or the docked cursor moves the wrong way. */
            g_cur_y = clampi(g_cur_y + (g_flip_y ? st.y : -st.y) / 4000, 0,
                             (int)SCREEN_H - 1);
    }
    held = padGetButtons(&g_pad);
    *px = g_cur_x;
    *py = g_cur_y;
    return (held & HidNpadButton_A) ? 1 : 0;
}

/* ---- keyboard ---------------------------------------------------------
 *
 * s3eKeyboardGetState was never implemented -- it fell through to the generic
 * stub and returned 0, which is why no physical button does anything even
 * though the game polls it thousands of times a session and registers two
 * s3eKeyboard callbacks.
 *
 * Mapping Switch buttons onto it needs the s3eKey codes this build actually
 * uses, and guessing them wrong is indistinguishable from no mapping at all.
 * So this reports which codes the game asks for; the mapping follows once the
 * list is known. The state answered stays 0 meanwhile, exactly as before, so
 * nothing changes behaviourally. */
static uint32_t g_key_state[512];

/* The codes this build polls, found by census: 5, 6, 99, 102. Rather than
 * guess which s3eKey constants they are, each is driven by a face button so
 * pressing one reveals what it does. Adjust once the semantics are known. */
static const struct { u64 button; uint32_t key; const char *name; }
g_key_map[] = {
    /* The game is touch-driven -- the Vita port works from its front
     * touchscreen alone -- so these are a convenience, not the main path.
     * 5 proceeds and 126 goes back, observed; the rest are unidentified. */
    { HidNpadButton_A,      5,   "A(confirm?)" },
    { HidNpadButton_B,      126, "B(back?)"    },
    { HidNpadButton_Up,     6,   "Up->6"       },
    { HidNpadButton_Down,   99,  "Down->99"    },
    { HidNpadButton_Left,   102, "Left->102"   },
    { HidNpadButton_Right,  24,  "Right->24"   },
};

/* s3eKeyState: 0 up, 1 pressed this frame, 2 down, 3 released this frame --
 * the same shape as the pointer states, which is the SDK's convention. */
static void key_update(GuestMem *mem) {
    static u64 prev;
    u64 held;
    unsigned i;
    padUpdate(&g_pad);
    held = padGetButtons(&g_pad);

    /* ZL+ZR cycles the touch Y convention live. Four combinations, and which
     * one is right is an empirical question about the game's two input layers
     * -- far quicker to feel than to reason about, and this avoids a reflash
     * or editing a file on the card for each try. ZL/ZR are deliberately not
     * mapped to any s3eKey, so the combo cannot collide with game input. */
    {
        static u64 prev_combo;
        u64 combo = held & (HidNpadButton_ZL | HidNpadButton_ZR);
        int both = combo == (HidNpadButton_ZL | HidNpadButton_ZR);
        int was_both = prev_combo == (HidNpadButton_ZL | HidNpadButton_ZR);
        if (both && !was_both) {
            g_flip_y = (g_flip_y + 1) & 3;
            printf("  [key  ] flip mode -> %d (single=%d multi=%d)\n",
                   g_flip_y, FLIP_SINGLE ? 1 : 0, FLIP_MULTI ? 1 : 0);
        }
        prev_combo = combo;
    }

    for (i = 0; i < sizeof g_key_map / sizeof g_key_map[0]; i++) {
        uint32_t k = g_key_map[i].key;
        int now = (held & g_key_map[i].button) != 0;
        int was = (prev & g_key_map[i].button) != 0;
        if (k >= 512)
            continue;
        g_key_state[k] = now ? (was ? 2u : 1u) : (was ? 3u : 0u);
        if (now != was) {
            /* s3eKeyboardEvent { m_Key, m_Pressed }, passed by address. The
             * first version handed the callback the key code itself as its
             * sysdata, so the game dereferenced address 5 and faulted. One
             * buffer per mapped button, because the queue is not drained until
             * s3eDeviceYield and two buttons can change in the same frame. */
            static uint32_t ev[sizeof g_key_map / sizeof g_key_map[0]];
            if (!ev[i])
                ev[i] = galloc(8);
            printf("  [key  ] %s %s -> s3eKey %u state %u\n",
                   g_key_map[i].name, now ? "down" : "up", (unsigned)k,
                   (unsigned)g_key_state[k]);
            if (ev[i]) {
                guest_st32(mem, ev[i] + 0, k);
                guest_st32(mem, ev[i] + 4, (uint32_t)now);
                cb_queue("s3eKeyboard", 0, ev[i]);
            }
        }
    }
    prev = held;
    (void)mem;
}

/* The game calls this once a frame, which is the natural place to sample the
 * pad -- the same contract s3ePointerUpdate has. */
static void hle_key_update(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    key_update(mem);
    cpu->r[0] = 0;
}

static void hle_key_getstate(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t key = cpu->r[0];
    (void)mem; (void)user;
    {   /* Distinct codes only, and the first 40 of them. */
        static uint16_t seen[40];
        static int nseen;
        int i;
        for (i = 0; i < nseen; i++)
            if (seen[i] == (uint16_t)key)
                break;
        if (i == nseen && nseen < 40 && key < 512) {
            seen[nseen++] = (uint16_t)key;
            printf("  [key  ] game polls s3eKey %u (0x%x)\n", (unsigned)key,
                   (unsigned)key);
        }
    }
    cpu->r[0] = (key < 512) ? g_key_state[key] : 0u;
}

/* void s3ePointerUpdate(void) -- verified: at its only call site (RVA
 * 0x0c6506) r0 is overwritten by `ldr r0,[r3]` immediately after, so the
 * return value is discarded. */
/* Reconcile the touchscreen against the previous frame and emit the touch
 * events the difference implies. Slots are held by finger id so a contact
 * keeps its touchID for its whole life, which is what the game tracks a stick
 * by -- reindexing them each frame would look like every finger lifting and
 * new ones landing elsewhere. */
static void touch_update(GuestMem *mem) {
    HidTouchScreenState ts;
    Touch now[MAX_TOUCH];
    int i, j, n = 0;

    memset(now, 0, sizeof now);
    if (g_touch_ready && hidGetTouchScreenStates(&ts, 1)) {
        for (i = 0; i < (int)ts.count && n < MAX_TOUCH; i++) {
            /* Ignore anything outside the rendered viewport. Clamping instead
             * turned a palm resting on the left letterbox into a permanent
             * contact pinned to x=0 -- it took slot 0 and never lifted, so the
             * game saw a virtual stick held at full deflection and every real
             * finger afterwards came in as id=1. That reads as jammed
             * controls, which is what "seems like a crash" was. */
            if ((int)ts.touches[i].x < (int)VIEW_X ||
                (int)ts.touches[i].x >= (int)(VIEW_X + VIEW_W))
                continue;
            now[n].active = 1;
            now[n].fid = ts.touches[i].finger_id;
            now[n].x = clampi(((int)ts.touches[i].x - (int)VIEW_X) *
                              (int)SCREEN_W / (int)VIEW_W, 0,
                              (int)SCREEN_W - 1);
            now[n].y = clampi((int)ts.touches[i].y * (int)SCREEN_H / (int)FB_H,
                              0, (int)SCREEN_H - 1);
            if (FLIP_MULTI)
                now[n].y = (int)SCREEN_H - 1 - now[n].y;
            now[n].raw_x = (int)ts.touches[i].x;
            now[n].raw_y = (int)ts.touches[i].y;
            /* Drive the polled getters straight from the first in-viewport
             * contact. Deriving them further downstream left them pinned at
             * the startup centre -- the game polls s3ePointerGetX from RVA
             * 08f567 and read a constant 240 for an entire session. Set them
             * here, where the coordinate is known good, rather than behind a
             * condition that has to hold. */
            if (n == 0) {
                g_tap_x = now[0].x;
                g_tap_y = FLIP_SINGLE ? (int)SCREEN_H - 1 - now[0].y
                                      : now[0].y;
            }
            n++;
        }
    }

    /* Releases first: a slot whose finger id is no longer present. */
    for (i = 0; i < MAX_TOUCH; i++) {
        int still = 0;
        if (!g_touch[i].active)
            continue;
        for (j = 0; j < n; j++)
            if (now[j].fid == g_touch[i].fid)
                still = 1;
        if (!still) {
            touch_fire(mem, i, 0);
            g_touch[i].active = 0;
        }
    }

    /* Then moves and presses, each finger keeping or claiming a slot. */
    for (j = 0; j < n; j++) {
        int slot = -1;
        for (i = 0; i < MAX_TOUCH; i++)
            if (g_touch[i].active && g_touch[i].fid == now[j].fid)
                slot = i;
        if (slot >= 0) {
            if (now[j].x != g_touch[slot].x || now[j].y != g_touch[slot].y) {
                g_touch[slot].x = now[j].x;
                g_touch[slot].y = now[j].y;
                touch_motion_fire(mem, slot);
            }
            continue;
        }
        for (i = 0; i < MAX_TOUCH && slot < 0; i++)
            if (!g_touch[i].active)
                slot = i;
        if (slot < 0)
            continue;                   /* more fingers than slots; ignore */
        g_touch[slot] = now[j];
        touch_fire(mem, slot, 1);
    }
}

static void hle_ptr_update(GuestCpu *cpu, GuestMem *mem, void *user) {
    static int last_motion_x = -1, last_motion_y = -1;
    int x = 0, y = 0, down;
    (void)user;

    down = input_poll(&x, &y);
    touch_update(mem);
    /* The polled getters must agree with the tracked contacts. input_poll only
     * looks at ts.touches[0] and gives up if that contact is outside the
     * viewport, so a palm on the bezel taking that index left g_tap_x/y frozen
     * at the startup centre -- and the game polls s3ePointerGetX from RVA
     * 08f567 for its hit-testing, so every menu tap was tested against (240,
     * 160) no matter where the finger was. touch_update scans every contact
     * and already filters the letterbox, so prefer what it tracked. */
    if (g_touch[0].active) {
        x = g_touch[0].x;
        y = g_touch[0].y;
        down = 1;
    }
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

/* s3ePointerGetX() takes no argument; s3ePointerGetTouchX(touchID) does, and
 * both were bound here, so every finger read back as the primary one. The
 * touch forms are split out below. */
/* Which position the game actually reads is the open question: it can take one
 * from the callback event struct, or poll these. Flipping the callback
 * coordinates changed nothing, which is what it would look like if the menus
 * poll instead. Log what these hand back, and from where. */
static void ptr_read_log(const char *what, uint32_t v, uint32_t lr) {
    static int shown;
    if (shown < 60) {
        shown++;
        printf("  [read ] %s -> %u  (caller RVA %06x)\n", what, (unsigned)v,
               (unsigned)lr);
    }
}

static void hle_ptr_getx(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = (uint32_t)g_tap_x;
    ptr_read_log("GetX", cpu->r[0], cpu->r[GUEST_LR] - g_img.load_base);
}
static void hle_ptr_gettouchx(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t id = cpu->r[0];
    (void)mem; (void)user;
    cpu->r[0] = (id < MAX_TOUCH && g_touch[id].active)
                    ? (uint32_t)g_touch[id].x : (uint32_t)g_tap_x;
    ptr_read_log("GetTouchX", cpu->r[0], cpu->r[GUEST_LR] - g_img.load_base);
}
static void hle_ptr_gettouchy(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t id = cpu->r[0];
    (void)mem; (void)user;
    cpu->r[0] = (id < MAX_TOUCH && g_touch[id].active)
                    ? (uint32_t)g_touch[id].y : (uint32_t)g_tap_y;
}
static void hle_ptr_gettouchstate(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t id = cpu->r[0];
    (void)mem; (void)user;
    cpu->r[0] = (id < MAX_TOUCH && g_touch[id].active)
                    ? (uint32_t)S3E_PTR_DOWN : (uint32_t)S3E_PTR_UP;
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
    /* Saying no here is what left the game with no in-game controls: it then
     * registers only the single-touch callbacks and its dual-stick scheme has
     * nothing to drive it. The Switch touchscreen reports up to 16 contacts. */
    case 4:  v = 1; break;              /* MULTI_TOUCH_AVAILABLE = yes */
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
/* ------------------------------------------------------------ frame profile
 *
 * Overclocking to 2400 MHz -- 2.35x the stock CPU -- moved the frame rate only
 * 9.6 -> 13.5. If interpreting guest instructions were the whole frame that
 * would have been close to linear, so most of a frame is going somewhere else.
 * This splits it: ticks spent inside HLE handlers (GL, EGL, file, everything
 * reached through the stub page) versus ticks spent interpreting, plus the
 * worst individual imports by name. Everything is 19.2 MHz system ticks. */
static uint64_t g_prof_hle_ticks;         /* inside handlers, this window */
static uint64_t g_prof_enter;             /* tick at the current handler entry */
static uint32_t g_prof_depth;             /* handlers can re-enter via callbacks */
static uint64_t g_prof_slot_ticks[512];
static uint32_t g_prof_slot_calls[512];
static uint32_t g_prof_slot;

static void hle_profile(uint32_t slot, int enter) {
    uint64_t now = armGetSystemTick();
    if (enter) {
        if (g_prof_depth++ == 0) {        /* only the outermost call is real */
            g_prof_enter = now;
            g_prof_slot = slot;
        }
        return;
    }
    if (--g_prof_depth == 0) {
        uint64_t d = now - g_prof_enter;
        g_prof_hle_ticks += d;
        if (g_prof_slot < 512) {
            g_prof_slot_ticks[g_prof_slot] += d;
            g_prof_slot_calls[g_prof_slot]++;
        }
    }
}

/* Scheduling of the interpreter thread itself; filled in at startup. */
static int32_t g_main_prio = -1;

/* Defined below, next to the Guest it reads. */
static void pc_profile_report(void);
static void pc_profile_clear(void);
static void i_profile_report(void);
static void i_profile_clear(void);
static void jit_report(void);

/* Reported every 300 frames rather than every frame: the point is the split,
 * and printing it per frame would itself distort the thing being measured. */
static void frame_profile_report(void) {
    static uint64_t window_start;
    uint64_t now = armGetSystemTick(), freq = armGetSystemTickFreq();
    uint64_t total, hle;
    int i, best[5], b;

    if (!window_start) {
        /* Clear the slot arrays too, not just the aggregate. Missing that made
         * the first report divide ticks accumulated since boot by one 17s
         * window, which read as glClear 12% / s3eFileRead 11% when the honest
         * steady-state answer is that handlers are near zero. */
        window_start = now;
        g_prof_hle_ticks = 0;
        memset(g_prof_slot_ticks, 0, sizeof g_prof_slot_ticks);
        memset(g_prof_slot_calls, 0, sizeof g_prof_slot_calls);
        /* The PC histogram too. Leaving it would charge the whole loading
         * phase to the first steady-state window, which is how the handler
         * split came to report glClear 12% on its first print. */
        pc_profile_clear();
        i_profile_clear();
        return;
    }
    total = now - window_start;
    hle = g_prof_hle_ticks;
    if (!total)
        return;

    printf("  [prof ] %llu ms/300f: handlers %llu%%, interpreting %llu%%\n",
           (unsigned long long)(total * 1000ull / freq),
           (unsigned long long)(hle * 100ull / total),
           (unsigned long long)((total - (hle < total ? hle : total)) * 100ull
                                / total));

    for (b = 0; b < 5; b++) {
        uint64_t bestv = 0;
        best[b] = -1;
        for (i = 0; i < 512; i++) {
            int seen = 0, k;
            for (k = 0; k < b; k++)
                if (best[k] == i) seen = 1;
            if (!seen && g_prof_slot_ticks[i] > bestv) {
                bestv = g_prof_slot_ticks[i];
                best[b] = i;
            }
        }
        if (best[b] < 0 || !bestv)
            break;
        printf("  [prof ]   %-28s %3llu%%  %u calls\n",
               slot_name((uint32_t)best[b]),
               (unsigned long long)(bestv * 100ull / total),
               (unsigned)g_prof_slot_calls[best[b]]);
    }

    memset(g_prof_slot_ticks, 0, sizeof g_prof_slot_ticks);
    memset(g_prof_slot_calls, 0, sizeof g_prof_slot_calls);
    g_prof_hle_ticks = 0;
    if (g_ctype_hits[0] || g_ctype_hits[1]) {
        printf("  [fast ] ctype: %u tolower, %u toupper\n",
               (unsigned)g_ctype_hits[0], (unsigned)g_ctype_hits[1]);
        ctype_report_callers(g_img.load_base);
        g_ctype_hits[0] = g_ctype_hits[1] = 0;
    }
    if (g_f32_hits[0] || g_f32_hits[1]) {
        printf("  [fast ] f32 guard: %u store, %u mul-store\n",
               (unsigned)g_f32_hits[0], (unsigned)g_f32_hits[1]);
        g_f32_hits[0] = g_f32_hits[1] = 0;
    }
    if (g_div_hits[0] || g_div_hits[1] || g_div_hits[2]) {
        printf("  [fast ] divide: %u uidiv, %u uidivmod, %u idiv\n",
               (unsigned)g_div_hits[0], (unsigned)g_div_hits[1],
               (unsigned)g_div_hits[2]);
        g_div_hits[0] = g_div_hits[1] = g_div_hits[2] = 0;
    }
    {   /* Which core we are on, and whether we stayed there.
         *
         * The guest is single-threaded -- 391 imports and not one thread,
         * mutex or atomic among them -- so no amount of threading can split
         * the instruction stream that is 98% of frame time. What CAN cost real
         * time is this thread sharing a core with system work or being
         * migrated between cores, and nothing here has ever set an affinity.
         * Report it before trying to fix it. */
        static int last_core = -1;
        static int migrations;
        int core = (int)svcGetCurrentProcessorNumber();
        if (last_core >= 0 && core != last_core)
            migrations++;
        last_core = core;
        printf("  [sched] core %d, %d migrations seen, prio %d\n",
               core, migrations, (int)g_main_prio);
    }
    jit_report();
    pc_profile_report();
    i_profile_report();
    window_start = now;
}

void egl_frame_presented(void) {
    g_presents++;
    tap_arm(g_presents);
    if (g_presents <= 3 || (g_presents % 100) == 0)
        printf("  [egl  ] frame %d presented via GL\n", g_presents);
    if ((g_presents % 300) == 0)
        frame_profile_report();
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

/* ------------------------------------------------------------ PC histogram */

/* Instruction mix. pcprof says which code is hot; this says what it is made
 * of. The host bench can report the same counters, but its 300M instructions
 * are the loading phase and come out ~100%% ARM library code -- gameplay is
 * Thumb, so the number that matters can only be taken here. */
static uint32_t g_iprof[GUEST_IPROF_N];

static void jit_report(void) {
    if (!g.jit && !g.jit_blocks)
        return;
    /* Coverage as a share of everything executed, which is the number that
     * actually tracks progress. Mean block length does not: lowering a branch
     * turns blocks that previously failed to compile into valid one-instruction
     * blocks, so the mean falls while coverage rises -- which is exactly what
     * r71 did, 1.67 down to 1.48 while covering strictly more. */
    if (g.executed)
        printf("  [jit  ] covering %llu%% of instructions\n",
               (unsigned long long)(g.jit_executed * 100ull / g.executed));
    printf("  [jit  ] %u blocks, %lluM insns via JIT, %llu verified, %u diverged\n",
           (unsigned)g.jit_blocks,
           (unsigned long long)(g.jit_executed / 1000000ull),
           (unsigned long long)g.jit_verify_blocks,
           (unsigned)g.jit_diverged);
    printf("  [jit  ] %llu bails (%llu retiring nothing), %llu insns lost to them\n",
           (unsigned long long)g.jit_bails,
           (unsigned long long)g.jit_bails_empty,
           (unsigned long long)g.jit_bail_lost);
    guest_jit_report_blockers(&g);
}

static void i_profile_clear(void) {
    memset(g_iprof, 0, sizeof g_iprof);
}

static void i_profile_report(void) {
    uint64_t tot = 0;
    unsigned i, k;
    for (i = 0; i < GUEST_IPROF_N; i++)
        tot += g_iprof[i];
    if (!tot)
        return;
    printf("  [iprof] instruction mix:\n");
    for (k = 0; k < 10; k++) {
        unsigned best = 0;
        uint32_t bv = 0;
        for (i = 0; i < GUEST_IPROF_N; i++)
            if (g_iprof[i] > bv) { bv = g_iprof[i]; best = i; }
        if (!bv)
            break;
        printf("  [iprof]   %-3s %02x  %2llu%%  %8lluk\n",
               best < 256 ? "T16" : best < 512 ? "T32" : "ARM",
               (unsigned)(best & 0xFFu),
               (unsigned long long)((uint64_t)bv * 100ull / tot),
               (unsigned long long)(bv / 1000u));
        g_iprof[best] = 0;
    }
    i_profile_clear();
    /* One report is all this is for, and the counter is not free: the null
     * check alone measured 3.9% on the host bench, which is a lot to pay
     * forever to answer a question once. Disarm it so every window after the
     * first is timed without it. */
    g.iprof = NULL;
    printf("  [iprof] counter disarmed; later windows are untaxed\n");
}

static void pc_profile_clear(void) {
    if (g.pcprof)
        memset(g.pcprof, 0, (size_t)g.pcprof_buckets * sizeof(uint32_t));
}

/* Coalesce runs of executed buckets back into functions and print the busiest.
 * Adjacency is what does the work here: a hot routine is a contiguous span of
 * 64-byte buckets, so printing raw buckets would split one function across a
 * dozen lines and hide it under something flatter. Gaps of up to four empty
 * buckets -- 64 bytes -- stay inside a span, since a cold error path in the
 * middle of a hot function is normal and breaking there would report its two
 * halves separately. */
static void pc_profile_report(void) {
    struct span { uint32_t lo, hi; uint64_t hits; } top[12], cur;
    uint32_t i, n = g.pcprof_buckets;
    uint64_t total = 0;
    int ntop = 0, k, j;

    if (!g.pcprof || !n)
        return;
    for (i = 0; i < n; i++)
        total += g.pcprof[i];
    if (!total)
        return;

    printf("  [pcprof] %lluM guest instructions, busiest code:\n",
           (unsigned long long)(total / 1000000ull));

    for (i = 0; i < n; ) {
        uint32_t gap = 0;
        if (!g.pcprof[i]) { i++; continue; }
        cur.lo = cur.hi = i;
        cur.hits = 0;
        while (i < n && gap <= 4) {
            if (g.pcprof[i]) {
                cur.hits += g.pcprof[i];
                cur.hi = i;
                gap = 0;
            } else {
                gap++;
            }
            i++;
        }
        for (k = 0; k < ntop; k++)
            if (cur.hits > top[k].hits)
                break;
        if (k < (int)(sizeof top / sizeof top[0])) {
            for (j = (ntop < (int)(sizeof top / sizeof top[0])
                      ? ntop : (int)(sizeof top / sizeof top[0]) - 1); j > k; j--)
                top[j] = top[j - 1];
            top[k] = cur;
            if (ntop < (int)(sizeof top / sizeof top[0]))
                ntop++;
        }
    }

    /* RVAs, because that is what the disassembler and every logged address in
     * this file are in -- the load base is added back only inside the guest. */
    for (k = 0; k < ntop; k++)
        printf("  [pcprof]   %06x..%06x %6u B %3llu%% %8lluk\n",
               (unsigned)(top[k].lo << 4),
               (unsigned)((top[k].hi << 4) + 15u),
               (unsigned)((top[k].hi - top[k].lo + 1) << 4),
               (unsigned long long)(top[k].hits * 100ull / total),
               (unsigned long long)(top[k].hits / 1000ull));

    pc_profile_clear();
}
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
    else if (!strcmp(nm, "s3ePointerGetTouchX"))
        g_slots[i].fn = hle_ptr_gettouchx;
    else if (!strcmp(nm, "s3ePointerGetTouchY"))
        g_slots[i].fn = hle_ptr_gettouchy;
    else if (!strcmp(nm, "s3ePointerGetTouchState"))
        g_slots[i].fn = hle_ptr_gettouchstate;
    else if (!strcmp(nm, "s3ePointerGetState"))
        g_slots[i].fn = hle_ptr_getstate;
    else if (!strcmp(nm, "s3ePointerGetX"))
        g_slots[i].fn = hle_ptr_getx;
    else if (!strcmp(nm, "s3ePointerGetY"))
        g_slots[i].fn = hle_ptr_gety;
    else if (!strcmp(nm, "s3eKeyboardGetState"))
        g_slots[i].fn = hle_key_getstate;
    else if (!strcmp(nm, "s3eKeyboardUpdate"))
        g_slots[i].fn = hle_key_update;
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
    {   /* The ICF lives at a FILE offset, and `file` is still mapped. */
        int n = 0;
        if (g_img.hdr.config_len &&
            (size_t)g_img.hdr.config_off + g_img.hdr.config_len <= size)
            n = s3e_config_load_icf((const char *)file + g_img.hdr.config_off,
                                    g_img.hdr.config_len);
        printf("cfg: %d keys from the game's own ICF (%u bytes)\n", n,
               (unsigned)g_img.hdr.config_len);
    }
    {   /* The BSS global holding the object read at RVA 0x23f228, resolved
         * from the GOT statically. Fixed in every run, so it can be watched
         * from the start rather than discovered.
         *
         * Disarmed now that the crash it was chasing is fixed. It is not free:
         * the interpreter only stamps mem.current_pc while a watch is armed,
         * and guest_wptr tests every store against the watched word, so
         * leaving it on costs about 3% for a diagnostic nothing is reading.
         * Re-arm by dropping watch.txt next to the NRO. */
        FILE *wf = fopen("sdmc:/switch/boz/watch.txt", "rb");
        if (!wf)
            wf = fopen("sdmc:/watch.txt", "rb");
        if (wf) {
            fclose(wf);
            g.mem.watch_addr = g_img.load_base + RVA_OBJ_GLOBAL;
            printf("watch: object global at %08x (RVA %06x)\n",
                   (unsigned)g.mem.watch_addr, (unsigned)RVA_OBJ_GLOBAL);
        }
    }
    {   /* The JIT is opt-in, and its self-check is opt-in on top of that.
         *
         * Off by default because it is the one component with no offline
         * oracle: run_boz.py single-steps the interpreter against Unicorn and
         * cannot follow a block-at-a-time execution, and the JIT only exists
         * on AArch64 so the host harness cannot run it at all. Until coverage
         * is broad enough to have been exercised for a long time, a file on
         * the card is the right switch.
         *
         *   jit.txt        compile and run hot blocks
         *   jitverify.txt  re-run every compiled block through the interpreter
         *                  and compare -- much slower, and the only way this
         *                  gets trustworthy
         *
         * Verification implies the JIT; asking for the check without the thing
         * being checked is a mistake worth silently fixing rather than
         * obeying. */
        static const char *jit_paths[] = {
            "sdmc:/switch/boz/jit.txt", "sdmc:/jit.txt" };
        static const char *ver_paths[] = {
            "sdmc:/switch/boz/jitverify.txt", "sdmc:/jitverify.txt" };
        /* Chaining is separate from the JIT and from its self-check, because
         * it is the one thing the self-check cannot cover: verification
         * re-runs a single block through the interpreter, and a chain is by
         * definition not a single block. So it gets its own switch, and it is
         * forced off whenever verification is on. */
        static const char *chain_paths[] = {
            "sdmc:/switch/boz/chain.txt", "sdmc:/chain.txt" };
        int want_jit = 0, want_ver = 0, want_chain = 0, k;
        for (k = 0; k < 2; k++) {
            FILE *f = fopen(jit_paths[k], "rb");
            if (f) { fclose(f); want_jit = 1; }
            f = fopen(ver_paths[k], "rb");
            if (f) { fclose(f); want_ver = 1; }
            f = fopen(chain_paths[k], "rb");
            if (f) { fclose(f); want_chain = 1; }
        }
        if (want_ver)
            want_jit = 1;
        if (want_chain)
            want_jit = 1;
        if (want_jit && guest_jit_init(&g)) {
            g.jit_verify = want_ver;
            g.jit_chain = want_chain && !want_ver;
            /* undo_active is NOT armed here: guest_run turns it on around the
             * block being checked and off again immediately. Left on globally
             * it logs every interpreter store too, and the rollback then undoes
             * ordinary execution. */
            printf("jit: enabled%s%s\n",
                   want_ver ? ", self-verifying (slow)" : "",
                   g.jit_chain ? ", chaining" :
                   (want_chain ? ", chaining suppressed by verify" : ""));
        } else if (want_jit) {
            printf("jit: requested but guest_jit_init failed\n");
        }
    }

    {   /* Allocator recycling is off unless the card asks for it back, so the
         * two behaviours can be compared without a rebuild. */
        static const char *paths[] = {"sdmc:/switch/boz/recycle.txt",
                                      "sdmc:/recycle.txt"};
        unsigned k;
        for (k = 0; k < 2; k++) {
            FILE *f = fopen(paths[k], "rb");
            char c = 0;
            if (!f)
                continue;
            if (fread(&c, 1, 1, f) == 1 && c == '0')
                g_recycle_freed = 0;
            fclose(f);
            break;
        }
        printf("heap: recycle freed blocks = %d\n", g_recycle_freed);
    }
    {   /* Touch Y origin, same shape of switch. */
        static const char *paths[] = {"sdmc:/switch/boz/flipy.txt",
                                      "sdmc:/flipy.txt"};
        unsigned k;
        for (k = 0; k < 2; k++) {
            FILE *f = fopen(paths[k], "rb");
            char c = 0;
            if (!f)
                continue;
            if (fread(&c, 1, 1, f) == 1 && (c == '0' || c == '1'))
                g_flip_y = (c == '1');
            fclose(f);
            break;
        }
        printf("input: flip touch Y = %d\n", g_flip_y);
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
    g.prof = hle_profile;       /* frame-time split; see frame_profile_report */
    /* Guest-PC histogram. Sized to the loaded image, so a PC outside it (stub
     * page, callback trampolines) falls out on the bounds test rather than
     * needing its own range. ~440 KB for a 7 MB image; if the allocation
     * fails the pointer stays NULL and the loop skips it. */
    g.pcprof_base = g_img.load_base;
    g.pcprof_buckets = (g_img.image_size + 15u) >> 4;
    g.iprof = g_iprof;
    g.pcprof = (uint32_t *)calloc(g.pcprof_buckets, sizeof(uint32_t));
    if (!g.pcprof)
        g.pcprof_buckets = 0;
    g.hle.slot = g_slots;
    g.hle.count = n + 1;
    startup_stage_write("07 imports bound");

    /* 24, not 16: ctype takes the count to 11, the divide block to 16, and
     * the vertex transform made that one past the end. Sized with room so
     * the next hook is not a silent overrun. */
    static GuestHook hooks[24];
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
    hooks[7].addr = g_img.load_base + RVA_IMG_PTR_LOAD;
    hooks[7].fn = watch_truncated_ptr;
    hooks[7].observe = 1;
    hooks[8].addr = g_img.load_base + RVA_STRUCT_COPY_STORE;
    hooks[8].fn = watch_struct_copy;
    hooks[8].observe = 1;
    g.hook_count = 9;

    /* ctype last, because it installs only if the image really is laid out the
     * way the disassembly said. Silence here means the game keeps running its
     * own tolower, which is slower but never wrong. */
    g_ctype_got = ctype_resolve_got(&g.mem, g_img.load_base);
    if (g_ctype_got) {
        hooks[9].addr = g_img.load_base + RVA_TOLOWER;
        hooks[9].fn = hook_tolower;
        hooks[10].addr = g_img.load_base + RVA_TOUPPER;
        hooks[10].fn = hook_toupper;
        g.hook_count = 11;
        printf("  [fast ] ctype table slot at %08x\n", (unsigned)g_ctype_got);
    } else {
        printf("  [fast ] ctype hooks not installed (literals did not read)\n");
    }

    {   /* Divide. Indices follow whatever the ctype block left hook_count at,
         * so the two blocks stay independent. */
        uint32_t d = g.hook_count;
        hooks[d + 0].addr = g_img.load_base + RVA_UIDIV;
        hooks[d + 0].fn = hook_uidiv;
        hooks[d + 1].addr = g_img.load_base + RVA_UIDIVMOD;
        hooks[d + 1].fn = hook_uidivmod;
        hooks[d + 2].addr = g_img.load_base + RVA_IDIV;
        hooks[d + 2].fn = hook_idiv;
        hooks[d + 3].addr = g_img.load_base + RVA_F32_GUARD;
        hooks[d + 3].fn = hook_f32_guard;
        hooks[d + 4].addr = g_img.load_base + RVA_F32_MUL_GUARD;
        hooks[d + 4].fn = hook_f32_mul_guard;
        hooks[d + 5].addr = g_img.load_base + RVA_VTX_TRANSFORM;
        hooks[d + 5].fn = hook_vtx_transform;
        hooks[d + 6].addr = g_img.load_base + RVA_VEC3_ADD16;
        hooks[d + 6].fn = hook_vec3_add16;
        hooks[d + 7].addr = g_img.load_base + RVA_NORMALISE;
        hooks[d + 7].fn = hook_normalise;
        g.hook_count = d + 8;
    }

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
    printf("  [fast ] vertex transform: %u hooked, %u skipped (unmapped)\n",
           (unsigned)g_vtx_hits, (unsigned)g_vtx_skips);
    printf("  [fast ] vec3 add: %u hooked, %u skipped; normalise: %u hooked, %u skipped\n",
           (unsigned)g_vadd_hits, (unsigned)g_vadd_skips,
           (unsigned)g_norm_hits, (unsigned)g_norm_skips);
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

    {   /* Record where the scheduler has put us. Nothing sets an affinity or a
         * priority anywhere in this program, so whatever this reports is the
         * default we inherited rather than a choice anyone made. */
        u64 mask = 0;
        s32 dummy = 0;
        svcGetThreadPriority(&g_main_prio, CUR_THREAD_HANDLE);
        svcGetThreadCoreMask(&dummy, &mask, CUR_THREAD_HANDLE);
        printf("sched: core %d, ideal %d, mask %llx, prio %d\n",
               (int)svcGetCurrentProcessorNumber(), (int)dummy,
               (unsigned long long)mask, (int)g_main_prio);
    }

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
