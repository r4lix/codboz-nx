/* Call of Duty: Black Ops Zombies on the Switch.
 *
 * Loads the Marmalade image, binds every GOT slot to the stub page, and runs
 * the ARM entry stub on Dynarmic (dynarmic_glue.cpp), with the s3e SDK
 * implemented here as HLE handlers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <unistd.h>

#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/iosupport.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <dirent.h>

#include "guest.h"
#include "dynarmic_glue.h"
#include <time.h>
#include "audio.h"
#include "settings.h"
#ifdef __SWITCH__
#include "net.h"
#include "menu.h"
#endif
#include "s3e_loader.h"
#include "s3e_files.h"
#include "s3e_config.h"
#ifdef __SWITCH__
#include "gl_thunks.h"
#endif

/* Compact guest address space: the regions total ~343 MB and the top of the
 * heap lands at 0x18000000. Nothing depends on the values beyond these
 * defines -- the image is position-independent and relocated at load, and
 * every durable reference in this project (hook addresses, the disassembly
 * mapping) is an RVA computed from load_base.
 *
 * The bottom 8 MB is deliberately left unmapped so a null guest pointer still
 * lands on nothing. */
#define IMAGE_BASE 0x00800000u      /* ~4.8 MB image */
#define STACK_BASE 0x01000000u
#define STACK_SIZE (1u << 20)
#define HEAP_BASE  0x04000000u      /* 320 MB -> top 0x18000000 */
#include "boz_build_id.h"   /* generated: hash of the sources in this build */
/* No __DATE__/__TIME__ here on purpose -- they record when THIS file was
 * compiled, which is not when the binary was built, and the difference is
 * exactly what made a working fix look like a failed copy. */
#define BOZ_BUILD_LABEL "codboz-" BOZ_BUILD_ID
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
static uint32_t g_tp_stub[5];        /* s3eTouchpad function table, as guest
                                      * addresses the game can actually call */
static uint32_t g_zc_stub[5];        /* s3eZeroConf (Local Wi-Fi), likewise */

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
    {
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
#define S3E_TOUCHPAD_HASH 0x1dbd7ce8u

static void hle_extgethash(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t i;
    (void)user;
    /* The Xperia Play analog pads.
     *
     * This is an Xperia Play title and it asks for s3eTouchpad by hash at
     * startup -- "error loading extension: s3eTouchpad" in the log is the game
     * being told no. That refusal is precisely why it falls back to drawing
     * virtual sticks on the screen, and why the pad had to be faked as touch.
     * Hand over a real table and the sticks can be delivered as sticks.
     *
     * Both the PortMaster port and, judging by its 960x544 pad geometry, the
     * Vita one do exactly this. */
    if (cpu->r[0] == S3E_TOUCHPAD_HASH && cpu->r[1] &&
        cpu->r[2] == sizeof g_tp_stub) {
        for (i = 0; i < 5; i++)
            guest_st32(mem, cpu->r[1] + 4 * i, g_tp_stub[i]);
        printf("  [tpad ] s3eTouchpad handed over\n");
        cpu->r[0] = 0;                  /* S3E_RESULT_SUCCESS */
        return;
    }
#ifdef __SWITCH__
    /* s3eZeroConf: Bonjour discovery for Local Wi-Fi. Refused, the game still
     * shows the menu but can never see a host or be seen as one. The table
     * entries are net.c's, in StartSearch, StopSearch, Publish,
     * UpdateTxtRecord, Unpublish order. */
    if (cpu->r[0] == NET_ZEROCONF_HASH && cpu->r[1] &&
        cpu->r[2] == sizeof g_zc_stub && g_zc_stub[0]) {
        for (i = 0; i < 5; i++)
            guest_st32(mem, cpu->r[1] + 4 * i, g_zc_stub[i]);
        printf("  [zconf] s3eZeroConf handed over\n");
        cpu->r[0] = 0;
        return;
    }
#endif
    /* A zero return makes the caller take the "extension missing" path and
     * then call the table entries anyway, so they still have to be callable. */
    for (i = 0; i + 4 <= cpu->r[2]; i += 4)
        guest_st32(mem, cpu->r[1] + i, g_ext_stub);
    cpu->r[0] = 1;
}

/* Callers dereference the result without a NULL check. */
static int gputs(GuestMem *m, uint32_t addr, const char *s);

static void hle_devstring(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t s;
    (void)user;
#ifdef __SWITCH__
    /* S3E_DEVICE_UNIQUE_ID. The Play Online server keys the player's account
     * on it, so it must be stable across launches (net.c keeps it on the
     * card); "unknown" for every player would be one shared account. */
    if (cpu->r[0] == 0x19u) {
        static uint32_t id;
        if (!id) {
            id = galloc(40);
            if (id)
                gputs(mem, id, net_device_id());
        }
        cpu->r[0] = id;
        return;
    }
#endif
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
 * repair_image_handler, which only runs on the first dispatcher call -- by then
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

/* Verified ARM EABI memory primitives in this exact image. Kept under
 * dynarmic because their cost is amortised over the bytes they move rather
 * than paid per call. The normal non-observe hook return preserves ARM/Thumb
 * interworking through LR. */
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

/* r24 ruled out a use-after-free: the free guard tried here never fired once,
 * in a run that reached the fault. These objects are never freed -- their
 * vtable is zeroed in place while the registry still points at them, which is
 * what the repair below exists to undo. */

static void repair_image_handler(GuestCpu *cpu, GuestMem *mem, void *user) {
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

    /* A registered handler whose vtable is not the one it was built with has
     * been overwritten; say so. Only a NULL vtable is repaired below --
     * anything else belongs to whoever owns the block now. */
    if (slot >= 0 && vtable != g_img.load_base + vtable_rva[slot]) {
        static int warned;
        if (warned < 8) {
            warned++;
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
 * malformed frame value from consuming minutes of guest time. This is
 * deliberately tied to the verified game RVA rather than changing general VFP
 * semantics. The hook is observe-mode; moving PC makes the interpreter execute
 * the first instruction after the loop in the same step. Not a speed-up but a
 * fix: skipping it hangs the game. */
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
    cpu->r[0] = (uint32_t)s3e_vfs_flush(cpu->r[0]);
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
/* 128: every sound channel registers its own generator and end-of-sample
 * handler, on top of the engine and UI listeners. */
#define MAX_CBS 128

typedef struct {
    char     kind[28];          /* import name minus the trailing "Register" */
    uint32_t id, fn, user;
    uint32_t chan;              /* s3eSoundChannel: its channel; else CB_NO_CHAN */
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
/* s3eSoundChannelRegister and its UnRegister take a leading CHANNEL argument
 * that the other Register imports do not, so every following argument sits one
 * register higher. The generic handlers read r0/r1/r2 as (id, fn, userData),
 * which for these two reads (channel, id, fn) instead -- and the log said so
 * plainly, once anyone looked:
 *
 *     [cb   ] s3eSoundChannel  id=0  fn=00000000  user=008d8821
 *     [cb   ] s3eSoundChannel  id=0  fn=00000001  user=008d76a9
 *
 * 0 and 1 are callback ids, not function pointers; the odd addresses in
 * `user` are Thumb function pointers, and they are the handlers the game
 * actually wants called. Registered as-is, `fn` holds 0 or 1 and dispatching
 * one would branch to address 0. Nothing queues sound events yet, so this has
 * been latent rather than fatal -- which is exactly why it needs fixing before
 * audio starts generating callbacks rather than after.
 *
 * The channel number itself is dropped: cb_find_fn keys on (kind, id, fn), so
 * two channels registering the SAME callback id and the same handler collapse
 * to one registration. Only channel 0 has ever appeared here. If a second one
 * shows up, the channel has to go into the key. */
static int cb_arg_shift(const char *nm) {
    return nm && (!strcmp(nm, "s3eSoundChannelRegister") ||
                  !strcmp(nm, "s3eSoundChannelUnRegister"));
}

static int cb_find_fn(const char *kind, uint32_t id, uint32_t fn) {
    int i;
    for (i = 0; i < g_cb_n; i++)
        if (g_cbs[i].used && g_cbs[i].id == id && g_cbs[i].fn == fn &&
            !strcmp(g_cbs[i].kind, kind))
            return i;
    return -1;
}

/* Channel callbacks are per CHANNEL. The game's sound manager registers a
 * generator (type 1) and an end-of-sample handler (type 0) on each channel it
 * allocates, with that channel's own sound instance as userData, and replaces
 * them on every allocation. Keyed by (kind, id, fn) like the other events,
 * every channel collapsed into one entry holding whichever instance registered
 * last -- invisible while only channel 0 was ever used, and wrong for anything
 * else. So a channel callback is found by (channel, type), used or not, which
 * also lets a re-registration reuse its slot instead of growing the table. */
#define CB_NO_CHAN 0xFFFFFFFFu
static unsigned g_chan_cb_log;

static int cb_find_chan(const char *kind, uint32_t chan, uint32_t id) {
    int i;
    for (i = 0; i < g_cb_n; i++)
        if (g_cbs[i].chan == chan && g_cbs[i].id == id &&
            !strcmp(g_cbs[i].kind, kind))
            return i;
    return -1;
}

static void hle_register(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t idx = (uint32_t)(uintptr_t)user;
    char kind[28];
    int i;
    const uint32_t a = (uint32_t)cb_arg_shift(slot_name(idx));
    const uint32_t r_id = cpu->r[a], r_fn = cpu->r[a + 1], r_ud = cpu->r[a + 2];
    const uint32_t chan = a ? cpu->r[0] : CB_NO_CHAN;
    (void)mem;
    cb_kind(slot_name(idx), kind, sizeof kind);
    /* Registrations accumulate. Only an identical (kind, id, fn) is treated as
     * a repeat -- re-registering the same handler must not stack it up -- but a
     * different function for the same event is an additional listener, not a
     * replacement. Overwriting here is what silently unhooked the engine's
     * pointer handler when the UI layer registered its own. */
    i = a ? cb_find_chan(kind, chan, r_id) : cb_find_fn(kind, r_id, r_fn);
    if (i < 0) {                            /* reuse a slot something freed */
        int k;
        for (k = 0; k < g_cb_n && i < 0; k++)
            if (!g_cbs[k].used)
                i = k;
    }
    if (i < 0 && g_cb_n < MAX_CBS)
        i = g_cb_n++;
    if (i >= 0) {
        memcpy(g_cbs[i].kind, kind, sizeof kind);
        g_cbs[i].id = r_id;
        g_cbs[i].fn = r_fn;
        g_cbs[i].user = r_ud;
        g_cbs[i].chan = chan;
        g_cbs[i].used = 1;
        /* The sound manager re-registers on every play, so channel callbacks
         * are reported only while there are few enough to read. */
        if (!a || g_chan_cb_log++ < 40)
            printf("  [cb   ] %-18s id=%-3u fn=%08x user=%08x%s%u\n", kind,
                   (unsigned)r_id, (unsigned)r_fn, (unsigned)r_ud,
                   a ? "  ch" : "", a ? (unsigned)chan : 0u);
    } else {
        printf("  [cb   ] %-18s id=%-3u fn=%08x DROPPED: table full\n", kind,
               (unsigned)r_id, (unsigned)r_fn);
    }
    cpu->r[0] = 0;                          /* S3E_RESULT_SUCCESS */
}

static void hle_unregister(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t idx = (uint32_t)(uintptr_t)user;
    char kind[28];
    int i;
    const uint32_t a = (uint32_t)cb_arg_shift(slot_name(idx));
    const uint32_t r_id = cpu->r[a], r_fn = cpu->r[a + 1];
    const uint32_t chan = a ? cpu->r[0] : CB_NO_CHAN;
    (void)mem;
    cb_kind(slot_name(idx), kind, sizeof kind);
    /* Only ever remove the exact handler named. Falling back to "the first
     * registration for this event" is how input dies mid-session: the game
     * tears down a UI listener and we unhook the engine's instead, after which
     * events are still generated and delivered to nothing. */
    /* A channel callback is named by (channel, type) alone: UnRegister takes
     * no function, so what sits in the function position here is garbage and
     * every channel unregistration used to report NOT FOUND. */
    i = a ? cb_find_chan(kind, chan, r_id) : cb_find_fn(kind, r_id, r_fn);
    if (i >= 0 && g_cbs[i].used) {
        g_cbs[i].used = 0;
        if (!a || g_chan_cb_log++ < 40)
            printf("  [cb   ] -%-17s id=%-3u fn=%08x\n", kind,
                   (unsigned)r_id, (unsigned)g_cbs[i].fn);
    } else if (!a || g_chan_cb_log++ < 40) {
        printf("  [cb   ] -%-17s id=%-3u fn=%08x NOT FOUND, keeping all\n",
               kind, (unsigned)r_id, (unsigned)r_fn);
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
static void timer_pump(void);   /* s3eTimerSetTimer callbacks; same reason */
static void snd_pump_finished(void);  /* defined with the sound state */
static void snd_pump_wedged(void);    /* retires channels nothing can drain */
static void net_pump_guest(void);    /* socket readiness and DNS results */
/* Renders a sound through the game's own generator callback; defined after
 * `g`, because it calls into the guest. Non-zero if it played something. */
static int snd_play_generated(GuestMem *mem, uint32_t ch, uint32_t start,
                              uint32_t samples, uint32_t rate, uint32_t volume);
/* Runs the end-of-sample handler for a drained channel and honours its answer;
 * defined after `g` for the same reason. */
static void snd_finish_channel(uint32_t ch);
static void snd_endinfo_readback(void);

/* ---- mixer self-test --------------------------------------------------
 *
 * The voice position was a 32-bit Q16 value, so its sample index wrapped at
 * 65536 and any longer sound played forever without ever draining -- which is
 * what made a repaired barricade (repair_00, buy_debris) loop. The game only
 * plays a sound that long when someone repairs a barricade, so proving the fix
 * would mean playing the game. This plays one on demand instead: a quiet tone
 * well past the old wrap point, timed from start to drain. */
#define SELFTEST_SAMPLES 100000u        /* > 65536, the old wrap point */
#define SELFTEST_CH      15u            /* the top voice; the game works up */
#define SELFTEST_RATE    22050u

static uint64_t g_selftest_t0;
static int      g_selftest_on;

static void snd_selftest_start(void) {
    int16_t *pcm = (int16_t *)malloc(SELFTEST_SAMPLES * sizeof(int16_t));
    unsigned i;
    if (!pcm) {
        printf("  [test ] mixer selftest: out of memory\n");
        return;
    }
    /* Audible enough to appear in a recording, quiet enough not to drown the
     * game while someone is playing it. */
    for (i = 0; i < SELFTEST_SAMPLES; i++)
        pcm[i] = (int16_t)(((i / 25u) & 1u) ? 600 : -600);
    snd_out_play_pcm(SELFTEST_CH, pcm, SELFTEST_SAMPLES, SELFTEST_RATE, 8);
    free(pcm);                          /* the mixer decoded into its own copy */
    g_selftest_t0 = armGetSystemTick();
    g_selftest_on = 1;
    printf("  [test ] mixer selftest: %u samples at %u Hz, expect %u ms\n",
           (unsigned)SELFTEST_SAMPLES, (unsigned)SELFTEST_RATE,
           (unsigned)((uint64_t)SELFTEST_SAMPLES * 1000ull / SELFTEST_RATE));
}

/* Prints once, when the voice drains. A build with the old wrap never prints:
 * the voice stays active forever, which is exactly the failure being tested. */
static void snd_selftest_poll(void) {
    unsigned ms, want;
    if (!g_selftest_on || snd_out_busy(SELFTEST_CH))
        return;
    ms = (unsigned)((armGetSystemTick() - g_selftest_t0) * 1000ull /
                    armGetSystemTickFreq());
    want = (unsigned)((uint64_t)SELFTEST_SAMPLES * 1000ull / SELFTEST_RATE);
    g_selftest_on = 0;
    printf("  [test ] mixer selftest: drained after %u ms (want %u) -- %s\n",
           ms, want, (ms + 250u >= want && ms <= want + 1500u) ? "PASS" : "FAIL");
}

static void hle_device_yield(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    g_yields++;
    if (g_yields <= 3 || (g_yields % 1000) == 0)
        printf("  [yield] #%d (%d callbacks registered)\n", g_yields, g_cb_n);
    snd_pump_finished();   /* end-of-sample callbacks; see below */
    snd_pump_wedged();
    snd_selftest_poll();
    cb_pump();
    timer_pump();
    net_pump_guest();
    snd_endinfo_readback();   /* what the handler wrote; defined below */
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
#define VIEW_W 1280u
#define VIEW_X ((FB_W - VIEW_W) / 2u)

static PadState g_pad;
static int g_quit;

/* ---- benchmark mode ---------------------------------------------------
 *
 * Every performance comparison this project has made was two hand-played
 * sessions, and not one of them measured the same work twice: paired runs
 * have come back with 890M against 305M guest instructions in the window
 * being compared, and 410 frames against 133 for the same instruction
 * count. Fixing the axis did not fix that, because the runs themselves
 * differed. No amount of care in the reporting can rescue a comparison
 * between two different workloads.
 *
 * The way out is already proved by the differential harness: this game is
 * deterministic given identical input -- interpreter, host and Unicorn
 * produce byte-identical frames out to present #350. So a run with no
 * REAL input is repeatable to the instruction, and the synthetic tap that
 * gets past TOUCH SCREEN TO START is itself deterministic (armed at a
 * fixed frame). Boot, tap synthetically, run to a fixed instruction count,
 * report the wall clock. One number, one variable, comparable across
 * builds and card flags and sessions.
 *
 * Real input is suppressed rather than merely discouraged, because a
 * single stray touch changes the instruction stream and silently turns the
 * benchmark back into two different workloads. Hands off anyway: + still
 * aborts, and that is the only button read. */
static int      g_bench;            /* the bench setting */
static uint64_t g_bench_target;     /* stop at this executed count */
static uint64_t g_bench_t0;         /* tick at the first guest instruction */
/* Set the moment the target is reached, so the measurement is taken THERE
 * and not after the frames spent announcing it. Read by fps_overlay in
 * gl_egl.c, which paints the banner. */
int             g_bench_done;
static uint64_t g_bench_ms, g_bench_instr;
static int      g_bench_until;      /* keep presenting until this frame */
static int      g_bench_presents;   /* presents AT the target, not at print time */

/* 2000M ended mid-load: 702 presents, still streaming assets. 5000M is
 * about four minutes and reaches settled gameplay. Overridable by putting
 * a number of millions in bench_million, so changing a run's length does
 * not need a rebuild -- and so a pair of runs can be made shorter while
 * something is being iterated on, then long again to confirm. */
#define BENCH_INSTR 5000000000ull
static int g_saw_real_input;         /* once true, the synthetic tap stands down */

#define SURF_BASE    0x01800000u   /* ~16.3 MB, room to 0x04000000 */

/* A page of guest-visible scratch for HLE structures the game writes into.
 *
 * s3eSoundChannel callbacks are handed a systemData POINTER and store through
 * it -- the end-of-sample handler writes +8 and +12. A bare channel number was
 * being passed instead, so every finished sound wrote into guest addresses 8
 * and 12. Nothing faulted, which is worse than if it had: the writes landed
 * somewhere real and quietly corrupted whatever lived there, with effects that
 * varied by screen and looked like a dozen different bugs.
 *
 * Its own small region below the image, so anything out of range still fails
 * cleanly. */
/* Set once the regions exist; the sound handlers sit far above the Guest
 * object in this file and cannot name it directly. */
static GuestMem *g_memp;

#define SCRATCH_BASE 0x00700000u
#define SCRATCH_SIZE 0x1000u
#define SCREEN_W     1280u
#define SCREEN_H     720u
#define SURF_BPP     2u
#define SURF_PIXTYPE 0x422u
#define SURF_FRAME   (SCREEN_W * SCREEN_H * SURF_BPP)
#define SURF_BYTES   (((SURF_FRAME + 0xFFFu) & ~0xFFFu) + (16u << 20))

/* Every pixel figure in the pad-to-touch code below was MEASURED, by capturing
 * real touches and reading the anchors and drag extents back out of the log.
 * They are only meaningful as a fraction of the screen, so say so, rather than
 * leaving bare integers that silently mean something else the moment the
 * reported surface size changes.
 *
 * The basis is the size they were measured at, so each reduces to the number
 * actually observed. It moved from 480x320 to 1280x720 when the surface did,
 * and that re-measurement was not optional: scaling the old numbers by the new
 * aspect put the left stick's deflection at 253 px when the largest drag a
 * hand actually makes is 242, so the contact was being pushed clean out of the
 * zone the stick lives in. The game pins its controls to the screen; where
 * they land is not something a ratio can be trusted to predict. */
#define PAD_BASE_W   1280
#define PAD_BASE_H   720
#define PX_W(v)      ((int)(v) * (int)SCREEN_W / PAD_BASE_W)
#define PX_H(v)      ((int)(v) * (int)SCREEN_H / PAD_BASE_H)

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
/* The live pointer position, tracked from real touch. Distinct from the
 * action tap's fixed target (g_act_x/g_act_y) -- these two were briefly
 * merged by a careless rename, and because C treats a tentative definition
 * and an initialised one as the SAME object it compiled cleanly and simply
 * made the action button tap wherever the last finger had been. */
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
/* Four slots for fingers, two more for the controller.
 *
 * The pad drives real touch contacts because that is the only input this build
 * understands for movement and aiming: it imports no s3eTouchpad, so the
 * analog path the Xperia Play used does not exist here. Giving the pad its own
 * slots rather than sharing means a thumb on the screen and a stick can be
 * used at the same time, which also makes this far easier to test. */
#define MAX_TOUCH  7
#define REAL_TOUCH 4          /* fingers claim 0..3; the pad owns 4, 5 and 6 */
#define PAD_SLOT_MOVE 4
#define PAD_SLOT_AIM  5
#define PAD_SLOT_TAP  6

typedef struct {
    int      active;
    int      x, y;
    int      raw_x, raw_y;           /* panel coordinate, kept for the log */
    uint32_t fid;                    /* Switch finger id, to track identity */
    /* ---- capture, for designing the controller mapping ----------------
     *
     * The virtual sticks float: the stick appears wherever a finger lands, so
     * there are no fixed zones to find. What a stick mapping does need is how
     * far a thumb actually travels from that anchor for full deflection --
     * that is the number that turns an analog position into pixels, and
     * guessing it gives controls that are subtly wrong in a way nobody can
     * debug afterwards. So each contact records where it started and how far
     * it got. */
    int      ax, ay;                 /* anchor: where this contact went down */
    int      max_r;                  /* furthest distance from the anchor */
    int      min_dx, max_dx;
    int      min_dy, max_dy;
    uint64_t t_down;
    int      samples;
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
    {
        Touch *t = &g_touch[slot];
        if (pressed) {
            t->ax = t->x;
            t->ay = t->y;
            t->max_r = 0;
            t->min_dx = t->max_dx = 0;
            t->min_dy = t->max_dy = 0;
            t->samples = 0;
            t->t_down = armGetSystemTick();
        } else {
            uint64_t f = armGetSystemTickFreq();
            unsigned ms = (unsigned)((armGetSystemTick() - t->t_down) * 1000ull /
                                     (f ? f : 1ull));
            printf("  [cap  ] id=%d anchor(%d,%d) %s  reach=%d"
                   "  dx=[%+d,%+d] dy=[%+d,%+d]  %u ms, %d moves\n",
                   slot, t->ax, t->ay,
                   t->ax < (int)(SCREEN_W / 2) ? "LEFT " : "RIGHT",
                   t->max_r, t->min_dx, t->max_dx, t->min_dy, t->max_dy,
                   ms, t->samples);
        }
    }
}

static void touch_motion_fire(GuestMem *mem, int slot) {
    uint32_t *buf = &g_ev_touch_motion[slot];
    {
        Touch *t = &g_touch[slot];
        int dx = t->x - t->ax, dy = t->y - t->ay;
        int r = dx * dx + dy * dy;
        t->samples++;
        if (dx < t->min_dx) t->min_dx = dx;
        if (dx > t->max_dx) t->max_dx = dx;
        if (dy < t->min_dy) t->min_dy = dy;
        if (dy > t->max_dy) t->max_dy = dy;
        /* Compare squared, store the root once: the summary wants pixels. */
        if (r > t->max_r * t->max_r) {
            int g = 0;
            while ((g + 1) * (g + 1) <= r)
                g++;
            t->max_r = g;
        }
    }
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
/* Touch Y origin: top-left, the same as the panel, with no flip anywhere.
 *
 * The old reasoning here -- that glViewport's bottom-left origin makes guest
 * y=0 the bottom of the screen -- is wrong. The viewport only places NDC; the
 * game's own ortho projection decides which way y grows, and the picture is
 * upright, so guest y is top-down like the panel. */
static int g_touch_ready;
static int g_real_down;              /* contact state on the previous update */
static int g_cur_x = (int)(SCREEN_W / 2), g_cur_y = (int)(SCREEN_H / 2);

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Touch markers for the GL overlay, in GL coordinates (origin bottom-left).
 *
 * Two of them, and that is the point: one where the finger physically is, one
 * where the game believes it is. If they sit on top of each other the mapping
 * is right; if one is the other's mirror the mapping is wrong and you can see
 * which axis without describing anything. Guessing at this from descriptions
 * has produced two wrong fixes already.
 *
 * The marks linger after release so the release position -- which is what UI
 * buttons act on, and where the bug lived -- stays visible long enough to see.
 */
/* Touch diagnostics: the two on-screen squares and the pointer logging.
 * Off unless touch_debug asks for it -- the markers are drawn every frame
 * and have no business appearing while anyone is playing. Kept rather than
 * deleted because seeing where the game thinks the finger is, next to where it
 * actually is, is what finally separated "wrong coordinates" from "right
 * coordinates, wrong widget". */
int g_touch_dbg;
static int g_mark_hold;
static int g_mark_fx, g_mark_fy, g_mark_gx, g_mark_gy;

int input_debug_marks(int *fx, int *fy, int *gx, int *gy);
int input_debug_marks(int *fx, int *fy, int *gx, int *gy) {
    if (g_mark_hold <= 0)
        return 0;
    g_mark_hold--;
    *fx = g_mark_fx; *fy = g_mark_fy;
    *gx = g_mark_gx; *gy = g_mark_gy;
    return 1;
}

static void mark_update(int active, int raw_x, int raw_y, int gx, int gy) {
    if (active && g_touch_dbg) {
        g_mark_fx = raw_x;
        g_mark_fy = (int)FB_H - 1 - raw_y;              /* panel -> GL */
        g_mark_gx = (int)VIEW_X + gx * (int)VIEW_W / (int)SCREEN_W;
        g_mark_gy = (int)FB_H - 1 - gy * (int)FB_H / (int)SCREEN_H;
        g_mark_hold = 90;                               /* ~3 s at 30 fps */
    }
}

static int input_poll(int *px, int *py) {
    HidTouchScreenState ts;
    u64 held;

    if (!g_touch_ready) {
        hidInitializeTouchScreen();
        g_touch_ready = 1;
    }
    if (
#ifdef __SWITCH__
        !menu_blocks_input() &&
#endif
        hidGetTouchScreenStates(&ts, 1) && ts.count > 0 &&
        (int)ts.touches[0].x >= (int)VIEW_X &&
        (int)ts.touches[0].x < (int)(VIEW_X + VIEW_W)) {
        *px = clampi(((int)ts.touches[0].x - (int)VIEW_X) * (int)SCREEN_W
                     / (int)VIEW_W, 0, (int)SCREEN_W - 1);
        *py = clampi((int)ts.touches[0].y * (int)SCREEN_H / (int)FB_H,
                     0, (int)SCREEN_H - 1);
        g_cur_x = *px;
        g_cur_y = *py;
        return 1;
    }

    /* Docked, there is no pointer at all.
     *
     * A left stick nudging an invisible cursor that A then clicked was removed
     * on purpose: nothing on screen showed where the cursor was, so it could
     * only be used by guessing, and it occupied the very stick the game wants
     * for movement. Controller support is being built properly instead --
     * in-game first, then menus -- and a half-working stand-in would compete
     * with it for the same inputs. */
    (void)held;
    *px = g_cur_x;
    *py = g_cur_y;
    return 0;
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
/* Where the action button taps.
 *
 * "TAP TO REPAIR BARRICADE" means exactly that: the action is a touch on the
 * object in the world, not a key and not a fixed button. No key code does it
 * -- 78 was sent, press and release, confirmed in the log, and nothing
 * happened. A real finger doing the repair landed at guest (245,232): low and
 * just right of centre, which is where a barricade sits when you are facing
 * one, and the prompt only appears when you are.
 *
 * So it is a tap there, on its own slot so it cannot disturb a stick being
 * held, and live-settable because buy prompts may want a different point.
 *
 * (245,232) worked for the prompt but ALSO fired the weapon whenever no
 * prompt was up, because the game splits the screen at SCREEN_W/2 = 240:
 * right half fires, left half is the floating movement stick. The action
 * hitbox straddles that line, so there is no point in neither zone. 230 is
 * one pixel-run to the left of the split -- it still repairs and buys, does
 * not fire, and the only cost is the movement stick flashing on screen for
 * the few frames the tap is held. That is the cheaper side effect, and it
 * goes away entirely once the on-screen controls are hidden. */
static int g_act_x = PX_W(613), g_act_y = PX_H(540);
static int g_act_frames;

static uint32_t g_key_state[512];

/* How often the game asks about each code, and how often we have sent it.
 *
 * The game echoes a poll straight after any key event it receives, so a code
 * appearing once in the log means nothing. What identifies the codes this
 * build actually uses is the ones it polls CONTINUOUSLY without being
 * prompted -- its own scan set -- and that only shows up as a count. Dumped on
 * demand (KEYS on the control socket) so the set can be compared between the
 * menu and gameplay, which use different ones. */
static uint32_t g_key_polls[512];

/* Minimum hold, in frames, for codes that want to be held rather than tapped.
 *
 * Reload (126) does not fire on a quick tap: the game either wants the key
 * held, or is waiting out its own double-tap window to see whether this is a
 * weapon swap instead. Those look identical from outside, and the ICF has no
 * timing key to tell them apart -- 17319 bytes of it and nothing about taps or
 * reload. So stretch a tap into a hold and let the game answer: if reload goes
 * instant, it wanted a hold; if it still lags, it is the double-tap window and
 * nothing here can shorten it.
 *
 * Live-settable, because the answer is one number and rebuilding per guess is
 * how this kind of thing gets abandoned half-done. */
static uint16_t g_min_hold[512];
static int      g_reload_hold = 12;      /* SND HOLD <frames>, ~200 ms at 60 */
/* How long B may be held before the game would read it as sprint. Six
 * frames is a hundred milliseconds: unmistakably a tap, and still long
 * enough for buy and repair, which worked on ordinary presses. */
static int      g_use_cap = 6;           /* SND TAPMAX <frames> */
/* Y pressed while moving sends use only once held this long (see key_update). */
static int      g_y_use_delay = 12;      /* SND YHOLD <frames> */
/* Aiming and sprinting are both TOGGLES in this game: one press of 74 starts
 * aiming and the next stops it, and 78 sprints for as long as it is down. On a
 * console pad the natural feel is the opposite of each -- hold the shoulder
 * button to aim, click the stick once to run. Both are converted here, and
 * both can be switched off live (SND AIMHOLD 0, SND RUNTOGGLE 0) so the game's
 * own behaviour is one command away rather than a rebuild. */
static int      g_aim_hold = 1;          /* SND AIMHOLD 0|1 */
static int      g_run_toggle = 1;        /* SND RUNTOGGLE 0|1 */
#define AIM_PULSE_FRAMES 8u              /* ~130 ms: a deliberate press */
#define AIM_GAP_FRAMES   6u              /* key up between two presses */
static uint32_t g_key_pulse;   /* SND KEY: one code to send next update */
static uint32_t g_key_sends[512];

/* s3eKeyState: 0 up, 1 pressed this frame, 2 down, 3 released this frame --
 * the same shape as the pointer states, which is the SDK's convention. */
/* The game speaks a gamepad protocol natively.
 *
 * It polls s3eKeyboardGetState thousands of times a session -- something this
 * port noticed early and could not explain -- because the Android build ships
 * Xperia Play support: a 2011 phone with physical controls. So buttons do not
 * need synthetic touch at all; they are key codes the game already understands.
 *
 * The codes are from the PortMaster loader for this same game
 * (github.com/Producdevity/cod-boz-port, MIT), which had already identified
 * them. Guessing them is exactly what failed here before: an earlier table
 * bound A and the d-pad to 5, 6, 99, 102 and 126 on the theory that pressing
 * one would reveal its meaning, and a wrong mapping is indistinguishable from
 * no mapping while being harder to reason about.
 *
 * Two codes per button, because the game uses different sets in menus and in
 * play: the ABS_* group drives menu navigation, the Xperia group drives the
 * game. Sending both on one press lets whichever is meaningful act and the
 * other be ignored, rather than needing to know which screen is up. If that
 * turns out to double-fire anywhere, splitting them by context is the fix. */
enum {
    XKEY_ALTERNATE_FIRE   = 9,
    XKEY_TACTICAL_GRENADE = 10,
    XKEY_CHANGE_WEAPON    = 11,
    XKEY_CROUCH_PRONE     = 12,
    XKEY_PAUSE            = 72,
    /* Unnamed in the reference Xperia table, which jumps 72 -> 74, but
     * very much real: a census in gamepad mode has the game polling it
     * 66834 times against 73589 for pause and 19956 for use, and never
     * once receiving it. A key scanned that hard is not vestigial. */
    XKEY_UNKNOWN_73       = 73,
    XKEY_AIM              = 74,
    XKEY_SHOOT            = 75,
    XKEY_ACTION_SPRINT    = 78,
    XKEY_MELEE            = 89,
    XKEY_THROW_GRENADE    = 90,
    XKEY_RELOAD           = 126,

    /* The ABS_* group (204-210) is what the reference loader uses, and this
     * build ignores it completely: a census showed those codes polled exactly
     * once each, immediately after we sent them, which is the game echoing an
     * event rather than scanning for one.
     *
     * What it DOES scan, continuously and unprompted, is these six -- 2148 to
     * 4298 polls apiece in fifty seconds of sitting in the menu, with nothing
     * mapped to them. Six codes is what a menu needs: four directions, a
     * confirm and a back, and 126 is known to open settings because pressing
     * it did.
     *
     * Which direction is which is the one thing the census cannot say, so the
     * four are assigned in the obvious order and corrected by pressing them.
     * That is a test, not a guess: the set is measured, only the order is
     * open. */
    MENU_KEY_CONFIRM = 5,
    MENU_KEY_UP      = 6,
    MENU_KEY_RIGHT   = 24,
    MENU_KEY_DOWN    = 99,
    MENU_KEY_LEFT    = 102,
    MENU_KEY_BACK    = 126,
};

/* Defined with the touchpad code below; the key map needs to know whether
 * the game is in gamepad mode. */
static int tp_engaged(void);

/* Black Ops on a console, as closely as this game allows.
 *
 * Nintendo letters sit where the PlayStation shapes do: A is Circle, B is
 * Cross, X is Triangle, Y is Square. So the console roles land as crouch on A,
 * swap on X and use/reload on Y, which is what this maps.
 *
 * The codes come from playing rather than from the reference header, and one
 * of them disagrees with it. 126 is named RELOAD there, but in this build a
 * single press SWAPS WEAPON and a double press reloads -- the combined button
 * the phone version shipped. It is also what opens settings in menus, which is
 * how it got mis-filed as a settings key earlier. So 126 goes on X, where the
 * console puts weapon swap, and it brings reload along with it.
 *
 * 78 is use: buy, repair, revive. It is on Y, where Square lives, and again on
 * B because that is the button the on-screen prompt points at while the game
 * is in Xperia gamepad mode. Two buttons on one code is safe now that state is
 * aggregated per code rather than per button -- before, the second entry
 * silently wiped the first one edge flags, which is why half of these
 * appeared dead no matter which code they were given.
 *
 * The D-pad keeps the reference actions. Nothing on the console maps there, so
 * it costs nothing and leaves 9 and 11 reachable for testing: 9 is alternate
 * fire, 11 the reference name for change weapon, which may or may not differ
 * from 126 in practice. */
/* `cap` is a maximum hold in frames, 0 for none.
 *
 * 78 is use AND sprint, and the game tells them apart the same way it tells
 * reload from weapon-swap: by how long the key is held. A tap uses, a hold
 * runs. Reload proved that model -- stretching a tap into a hold made it fire
 * instantly -- so the inverse applies here. Cap B so it can never read as a
 * hold, and leave L3 uncapped so sprint still has a home. One code, two
 * buttons, two behaviours, which only works at all because key state is
 * aggregated per code now. */
static const struct { u64 button; uint32_t game, alt; const char *name; int cap; }
g_key_map[] = {
    { HidNpadButton_ZR,      XKEY_SHOOT,            0, "ZR shoot"    , 0 },
    { HidNpadButton_ZL,      XKEY_AIM,              0, "ZL aim"      , 0 },

    /* Square on a PlayStation pad is BOTH use and reload, and this game splits
     * those across two codes -- 78 use, 126 reload -- so Y sends both.
     *
     * They need opposite timing, which is the whole reason this works. 78 is
     * capped so it stays a tap and never becomes sprint; 126 gets the minimum
     * hold that makes reload instant. One press, both behaviours, each with
     * the duration the game wants to see. */
    { HidNpadButton_Y,       XKEY_ACTION_SPRINT, XKEY_RELOAD, "Y use/reload", 0 },
    /* B is left FREE on purpose, held for a jump if one can be found.
     *
     * Cross is jump on a PlayStation pad, and nothing here does that yet. The
     * key census makes the odds look poor rather than open: every code the
     * game polls is now accounted for -- 9 alternate fire, 10 tactical, 11
     * change weapon, 12 crouch, 72 pause, 74 aim, 75 shoot, 78 use/sprint,
     * 89 melee, 90 grenade, 126 reload/swap, 5/6/24/99/102 menu directions,
     * and 73 which is scanned 66834 times a session and does nothing in any
     * state worth trying. None of them jumps.
     *
     * So if this build can jump at all, it is not through s3eKeyboard, and
     * binding a guess here would only hide that. Left empty until there is
     * something real to put in it. */
    { HidNpadButton_B,       0,                     0, "B free"      , 0 },
    /* Triangle swaps weapon on a console in ONE press, which 126 will not do.
     * 11 is the reference name for change-weapon and has never once been sent
     * in any session here, so this is the test as much as the binding. If it
     * does nothing, swap stays a double-tap of Y and this goes back to 126. */
    { HidNpadButton_X,       XKEY_CHANGE_WEAPON,    0, "X weapon"    , 0 },
    { HidNpadButton_A,       XKEY_CROUCH_PRONE,     0, "A crouch"    , 0 },

    { HidNpadButton_R,       XKEY_THROW_GRENADE,    0, "R grenade"   , 0 },
    { HidNpadButton_L,       XKEY_TACTICAL_GRENADE, 0, "L tactical"  , 0 },
    { HidNpadButton_StickR,  XKEY_MELEE,            0, "RS melee"    , 0 },
    { HidNpadButton_StickL,  XKEY_ACTION_SPRINT,    0, "LS sprint"   , 0 },
    { HidNpadButton_Plus,    XKEY_PAUSE,            0, "+ pause"     , 0 },

    { HidNpadButton_Up,      XKEY_TACTICAL_GRENADE, 0, "Up tactical" , 0 },
    { HidNpadButton_Down,    XKEY_CROUCH_PRONE,     0, "Down crouch" , 0 },
    { HidNpadButton_Left,    XKEY_ALTERNATE_FIRE,   0, "Left altfire", 0 },
    { HidNpadButton_Right,   XKEY_RELOAD,           0, "Right reload", 0 },
};

#define KEY_MAP_N (sizeof g_key_map / sizeof g_key_map[0])

/* s3eKeyState is a set of BITFLAGS, not a sequence.
 *
 *     DOWN = 1, PRESSED = 2, RELEASED = 4
 *
 * DOWN persists while held; PRESSED and RELEASED are edges that last one
 * update and are then cleared. This was encoded as 0/1/2/3 -- up, pressed,
 * down, released -- which is wrong in the worst possible way: while a button
 * was HELD it reported 2, meaning PRESSED, so the game saw a fresh press every
 * frame. Settings opened and shut ~50 times a second and repeated presses
 * crashed the process. And a real press reported 1, DOWN with no PRESSED bit,
 * so anything triggered on an edge -- the grenades -- never fired at all.
 *
 * Encoding confirmed against the PortMaster loader rather than assumed again. */
enum { KEY_DOWN = 1u, KEY_PRESSED = 2u, KEY_RELEASED = 4u };
/* Drive one CODE, not one button.
 *
 * The state array is indexed by key code, but several buttons deliberately
 * send the same one -- B and L3 are both ACTION, X and R are both GRENADE.
 * Scanning per button and clearing the edge flags on each call meant the LAST
 * entry for a code overwrote whatever an earlier one had just set: press B,
 * index 2 sets PRESSED, then index 13 (L3, not held) clears it again, and the
 * game polls a key that is DOWN with no edge. Press L3 and nothing follows it,
 * so the edge survives.
 *
 * That is the whole reason L3 rebuilt a barricade and B, sending the identical
 * code, did nothing -- and why Y appeared dead earlier for exactly the same
 * reason. The codes were right the entire time; the scan order was eating
 * them. So edges are cleared once per update, before anything is set, and a
 * code is down if ANY button bound to it is down. */
static void key_set_code(GuestMem *mem, uint32_t k, int now, int was) {
    /* One event buffer per code. The queue is not drained until
     * s3eDeviceYield, so sharing one would let a second change overwrite the
     * first before either is delivered. The event is { m_Key, m_Pressed }
     * passed BY ADDRESS -- handing the callback the code itself faulted the
     * guest, which is how that was learned. */
    static uint32_t ev[512];
    if (!k || k >= 512)
        return;
    if (now && !was)
        g_key_state[k] |= KEY_DOWN | KEY_PRESSED;
    else if (!now && was)
        g_key_state[k] = (g_key_state[k] & ~KEY_DOWN) | KEY_RELEASED;
    else if (now)
        g_key_state[k] |= KEY_DOWN;
    if (now == was)
        return;
    if (!ev[k])
        ev[k] = galloc(8);
    if (!ev[k])
        return;
    g_key_sends[k]++;
    guest_st32(mem, ev[k] + 0, k);
    guest_st32(mem, ev[k] + 4, (uint32_t)now);
    cb_queue("s3eKeyboard", 0, ev[k]);
}


static void key_update(GuestMem *mem) {
    static u64 prev;
    static unsigned shown;
    static uint8_t was_down[512];
    static unsigned press_frames[KEY_MAP_N];
    uint8_t now_down[512];
    u64 held;
    unsigned i, k;
    int moving;
    static unsigned still_frames;   /* updates since the left stick was last pushed */
    static int y_use_ok;            /* latched when Y goes down, kept for the press */

    padUpdate(&g_pad);
    held = padGetButtons(&g_pad);
#ifdef __SWITCH__
    /* The settings menu (hold - for 2 s) reads the pad before the game does.
     * While it is open, or until the buttons that closed it are released, the
     * game sees an idle controller: every key it holds is released below like
     * any other button going up. */
    {
        const HidAnalogStickState mls = padGetStickPos(&g_pad, 0);
        menu_input(held, mls.x, mls.y);
    }
    if (menu_blocks_input())
        held = 0;
#endif

    /* Y's use code (78) is also SPRINT, and the game tells them apart by
     * whether you are moving, not by how long the key is down: while walking,
     * a tap of any length starts a one-second run and the reload never
     * happens. The tap cap cannot fix that. So a Y press that starts while the
     * left stick is pushed -- or within STILL_FRAMES of it, while the character
     * is still coasting to a stop -- is reload only; a press that starts from
     * standing still is use + reload as before. 6000 is the stick deadzone
     * used below (PAD_DEADZONE).
     *
     * The decision is LATCHED when Y goes down. Checking movement every frame
     * instead flipped the use code on mid-press whenever the stick was let go,
     * which the game read as a sprint started during the coast -- and left the
     * running animation playing on a character that had already stopped. */
#define STILL_FRAMES 20u
    {
        HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
        moving = ls.x > 6000 || ls.x < -6000 || ls.y > 6000 || ls.y < -6000;
#ifdef __SWITCH__
        if (menu_blocks_input())
            moving = 0;             /* the stick is navigating the menu */
#endif
        if (moving)
            still_frames = 0;
        else if (still_frames < 0xFFFFu)
            still_frames++;
    }

    /* Which CODES are down, from every button bound to them. */
    memset(now_down, 0, sizeof now_down);
    for (i = 0; i < KEY_MAP_N; i++) {
        int cap;
        if (!(held & g_key_map[i].button)) {
            press_frames[i] = 0;
            continue;
        }
        if (press_frames[i] < 0xFFFFu)
            press_frames[i]++;
        /* Driven from the edges further down instead of straight from the
         * button: hold-to-aim and press-to-run. */
        if ((g_aim_hold && g_key_map[i].button == HidNpadButton_ZL) ||
            (g_run_toggle && g_key_map[i].button == HidNpadButton_StickL))
            continue;
        /* Past its cap the PRIMARY code stops contributing, so a long squeeze
         * still reaches the game as a short tap. The second code is not
         * capped: Y carries use and reload together, and they want opposite
         * treatment -- use must stay a tap so it never reads as sprint, while
         * reload wants the hold that makes it fire instantly. */
        cap = g_key_map[i].button == HidNpadButton_Y ? g_use_cap
                                                    : g_key_map[i].cap;
        if (g_key_map[i].button == HidNpadButton_Y && press_frames[i] == 1)
            y_use_ok = !moving && still_frames >= STILL_FRAMES;
        if (g_key_map[i].button == HidNpadButton_Y && !y_use_ok) {
            /* Pressed on the move: a TAP is reload only, and HOLDING past
             * g_y_use_delay adds use, so buying and repairing still work while
             * walking -- the way Square is held on a console. Held over
             * nothing, that is a sprint for exactly as long as it is held,
             * which a deliberate hold is asking for; the accidental one-second
             * run on a tap is what this removes. */
            if (press_frames[i] > (unsigned)g_y_use_delay &&
                g_key_map[i].game && g_key_map[i].game < 512)
                now_down[g_key_map[i].game] = 1;
        } else if (!(cap > 0 && press_frames[i] > (unsigned)cap) &&
                   g_key_map[i].game && g_key_map[i].game < 512) {
            now_down[g_key_map[i].game] = 1;
        }
        if (g_key_map[i].alt && g_key_map[i].alt < 512)
            now_down[g_key_map[i].alt] = 1;
    }

    /* Hold ZL to aim: the game toggles aiming on each press of 74, so holding
     * is that toggle driven from BOTH edges -- one pulse when ZL goes down,
     * another when it comes up. A pulse rather than a single frame because the
     * game samples key state once a frame and a one-frame press can fall
     * between two samples. */
    /* Tracked as STATE, not edges. Retriggering one pulse on each edge merged
     * a quick tap's press and release into a single press: the release landed
     * while the "aim on" pulse was still down, the game saw one toggle, and the
     * player stayed aimed until ZL was pressed again. Now the game's aim state
     * is followed as we believe it to be, and a separate press -- down, then a
     * gap up so the game sees two -- is sent whenever it differs from ZL. */
    if (g_aim_hold) {
        static int game_aiming;             /* what our toggles have left it at */
        static unsigned phase;              /* frames left in the current step */
        static int pressing;                /* 1 while 74 is down, 0 in the gap */
        const int want = (held & HidNpadButton_ZL) != 0;
        if (phase) {
            phase--;
            if (pressing)
                now_down[XKEY_AIM] = 1;
            if (!phase && pressing) {
                pressing = 0;
                game_aiming = !game_aiming; /* the press is complete */
                phase = AIM_GAP_FRAMES;     /* key up long enough to see */
            }
        } else if (want != game_aiming) {
            pressing = 1;
            phase = AIM_PULSE_FRAMES;
            now_down[XKEY_AIM] = 1;
        }
    }

    /* Click L3 to run: latch 78 down until L3 is clicked again. It has to
     * unlatch when the character stops, because 78 is also USE -- held over
     * nothing it buys and repairs instead of sprinting, which is the same
     * collision the Y handling above works around. */
    if (g_run_toggle) {
        static int run_latched;
        if ((held & HidNpadButton_StickL) && !(prev & HidNpadButton_StickL))
            run_latched = !run_latched;
        if (!moving && still_frames >= STILL_FRAMES)
            run_latched = 0;
        if (run_latched)
            now_down[XKEY_ACTION_SPRINT] = 1;
    }

    /* Stretch a tap into a hold for any code that asks for one. The key stays
     * down for the remainder of its window even after the button is released,
     * so a quick press reads as a deliberate one. */
    g_min_hold[XKEY_RELOAD] = (uint16_t)(g_reload_hold > 0 ? g_reload_hold : 0);
    {
        static uint16_t hold_left[512];
        for (k = 1; k < 512; k++) {
            if (now_down[k]) {
                if (g_min_hold[k])
                    hold_left[k] = g_min_hold[k];
            } else if (hold_left[k]) {
                hold_left[k]--;
                now_down[k] = 1;
            }
        }
    }

    /* Edges last exactly one update, and are cleared before anything sets
     * them so two buttons sharing a code cannot wipe each other. */
    for (k = 1; k < 512; k++)
        g_key_state[k] &= ~(KEY_PRESSED | KEY_RELEASED);

    /* Per-button work: the log line, and the fallback action tap. */
    for (i = 0; i < KEY_MAP_N; i++) {
        int now = (held & g_key_map[i].button) != 0;
        int was = (prev & g_key_map[i].button) != 0;
        /* Only worth doing when the game does NOT know it has a pad. With the
         * touchpad answered the action is a real button, and an extra screen
         * tap is just a stray touch -- one that fires the weapon if it lands
         * on the right half. */
        if (now && !was && g_key_map[i].button == HidNpadButton_B &&
            !tp_engaged())
            g_act_frames = 5;          /* ~80 ms, matching a real tap */
        if (now != was && shown < 40u) {
            shown++;
            printf("  [key  ] %-11s %s -> game %u / alt %u\n",
                   g_key_map[i].name, now ? "down" : "up  ",
                   (unsigned)g_key_map[i].game, (unsigned)g_key_map[i].alt);
        }
    }

    for (k = 1; k < 512; k++)
        if (now_down[k] || was_down[k])
            key_set_code(mem, k, now_down[k], was_down[k]);
    memcpy(was_down, now_down, sizeof was_down);

    /* A code asked for over the control socket, delivered as a real press and
     * release so the game sees exactly what a button would produce. */
    if (g_key_pulse) {
        static uint32_t pulsing;
        if (pulsing) {
            key_set_code(mem, pulsing, 0, 1);
            pulsing = 0;
            g_key_pulse = 0;
        } else {
            pulsing = g_key_pulse;
            key_set_code(mem, pulsing, 1, 0);
        }
    }
    prev = held;
}

/* The game calls this once a frame, which is the natural place to sample the
 * pad -- the same contract s3ePointerUpdate has. */
/* s3eKeyboardGetInt(property). The game asks once at startup, and until now
 * that fell through to the default stub and answered 0 -- which for a
 * "is there a keyboard" style property means no, and would leave every key
 * ignored no matter how well mapped.
 *
 * Answering 1 is provisional: the property ids are not known here, so each is
 * reported on first sight rather than assumed. If the game misbehaves, the log
 * says exactly which property it asked about and the answer can be narrowed to
 * that one. */
static void hle_key_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0];
    (void)mem; (void)user;
    if (prop < 32u && !(seen & (1u << prop))) {
        seen |= 1u << prop;
        printf("  [key  ] s3eKeyboardGetInt(%u) -> 1\n", (unsigned)prop);
    }
    cpu->r[0] = 1;
}

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
    if (key < 512)
        g_key_polls[key]++;
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
/* ---- s3eTouchpad: the sticks as sticks ---------------------------------
 *
 * The Xperia Play has two analog pads and this game was built for it. The
 * geometry below is the reference port, which reads like it came from the Vita
 * one: a 960x544 pad, the move stick centred a fifth of the way across with a
 * radius to match, and the look stick centred four fifths across with a much
 * smaller radius, which is what sets turn sensitivity.
 *
 * This is a different input DEVICE from the screen, which is the entire point.
 * Faking the pad as touch means living with the screen split the game applies
 * to touches -- left half moves, right half fires -- so the action button
 * could not be pressed without shooting, and a floating stick had to be
 * anchored somewhere and stayed visible. None of that applies to a pad the
 * game knows is a pad. */
/* The Xperia axis deadzone, which the reference keeps separate from the one
 * it uses for ordinary buttons. Same value as PAD_DEADZONE, declared here
 * because that one belongs to the synthetic-touch code further down. */
#define TP_DEADZONE 6000
#define TP_W      960
#define TP_H      544
#define TP_COUNT  2

static int      g_tp_active[TP_COUNT];
static int      g_tp_x[TP_COUNT], g_tp_y[TP_COUNT];
static uint32_t g_ev_tp_btn[TP_COUNT], g_ev_tp_mot[TP_COUNT];
static int      g_tp_on = 1;        /* SND TPAD 0|1 */
/* Look sensitivity is the pad RADIUS, not a speed: the game turns at a rate
 * set by how far from centre the contact sits, so a bigger radius turns
 * faster. The reference uses width/8; live-settable because it is pure feel
 * and rebuilding once per guess is how tuning gets abandoned half-done. */
static int      g_tp_look_r = TP_W / 8;   /* SND TPLOOK <radius> */
/* Vertical radius, separate: the game turns more slowly up and down than
 * sideways for the same pad offset, so one shared radius left pitch sluggish
 * at any setting that made yaw feel right. */
static int      g_tp_look_ry = TP_W / 8;

/* How many touchpad handlers the game has actually installed. Zero means it
 * ignored the extension, and the synthetic-touch path has to stay in charge:
 * offering the extension is not the same as the game choosing to use it. */
static int tp_listeners(void) {
    int i, n = 0;
    for (i = 0; i < g_cb_n; i++)
        if (g_cbs[i].used && !strcmp(g_cbs[i].kind, "s3eTouchpad"))
            n++;
    return n;
}

static int tp_engaged(void) {
    return g_tp_on && tp_listeners() > 0;
}

static void hle_tp_register(GuestCpu *cpu, GuestMem *mem, void *user) {
    int i;
    (void)mem; (void)user;
    i = cb_find_fn("s3eTouchpad", cpu->r[0], cpu->r[1]);
    if (i < 0 && g_cb_n < MAX_CBS)
        i = g_cb_n++;
    if (i >= 0) {
        snprintf(g_cbs[i].kind, sizeof g_cbs[i].kind, "s3eTouchpad");
        g_cbs[i].id = cpu->r[0];
        g_cbs[i].fn = cpu->r[1];
        g_cbs[i].user = cpu->r[2];
        g_cbs[i].used = 1;
        printf("  [tpad ] register id=%u fn=%08x user=%08x  (%d listeners)\n",
               (unsigned)cpu->r[0], (unsigned)cpu->r[1], (unsigned)cpu->r[2],
               tp_listeners());
    }
    cpu->r[0] = 0;
}

static void hle_tp_unregister(GuestCpu *cpu, GuestMem *mem, void *user) {
    int i;
    (void)mem; (void)user;
    i = cb_find_fn("s3eTouchpad", cpu->r[0], cpu->r[1]);
    if (i >= 0) {
        g_cbs[i].used = 0;
        printf("  [tpad ] unregister id=%u\n", (unsigned)cpu->r[0]);
    }
    cpu->r[0] = 0;
}

static void hle_tp_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static unsigned shown;
    uint32_t k = cpu->r[0];
    (void)mem; (void)user;
    cpu->r[0] = k == 0 ? 1u                     /* the pad exists */
              : k == 1 ? (uint32_t)TP_W
              : k == 2 ? (uint32_t)TP_H
              : (uint32_t)-1;
    if (shown < 8) {
        shown++;
        printf("  [tpad ] GetInt(%u) -> %d\n", (unsigned)k, (int)cpu->r[0]);
    }
}

static void tp_button(GuestMem *mem, int id, int pressed) {
    uint32_t *buf = &g_ev_tp_btn[id];
    if (!*buf)
        *buf = galloc(16);
    if (!*buf)
        return;
    guest_st32(mem, *buf +  0, (uint32_t)id);
    guest_st32(mem, *buf +  4, (uint32_t)pressed);
    guest_st32(mem, *buf +  8, (uint32_t)g_tp_x[id]);
    guest_st32(mem, *buf + 12, (uint32_t)g_tp_y[id]);
    cb_queue("s3eTouchpad", 0, *buf);
}

static void tp_motion(GuestMem *mem, int id) {
    uint32_t *buf = &g_ev_tp_mot[id];
    if (!*buf)
        *buf = galloc(12);
    if (!*buf)
        return;
    guest_st32(mem, *buf + 0, (uint32_t)id);
    guest_st32(mem, *buf + 4, (uint32_t)g_tp_x[id]);
    guest_st32(mem, *buf + 8, (uint32_t)g_tp_y[id]);
    cb_queue("s3eTouchpad", 1, *buf);
}

/* One pad, from one stick. Centre plus deflection, pressed while off-centre
 * and released when it returns -- the pad is touched exactly while the stick
 * is held, which is what the hardware being emulated would report.
 *
 * SDL measures a stick y downward and libnx measures it upward, so the sign
 * flips here; the reference port needs no such flip because SDL already agrees
 * with the pad coordinate system. */
static void tp_stick(GuestMem *mem, int id, int ax, int ay,
                     int cx, int cy, int rx, int ry) {
    int x, y;
    int mx = ax < 0 ? -ax : ax, my = ay < 0 ? -ay : ay;

    if (mx <= TP_DEADZONE && my <= TP_DEADZONE) {
        if (g_tp_active[id]) {
            g_tp_active[id] = 0;
            tp_button(mem, id, 0);
        }
        return;
    }
    x = clampi(cx + ax * rx / 32767, 0, TP_W - 1);
    y = clampi(cy - ay * ry / 32767, 0, TP_H - 1);
    if (!g_tp_active[id]) {
        g_tp_active[id] = 1;
        g_tp_x[id] = x;
        g_tp_y[id] = y;
        tp_motion(mem, id);
        tp_button(mem, id, 1);
        return;
    }
    if (g_tp_x[id] != x || g_tp_y[id] != y) {
        g_tp_x[id] = x;
        g_tp_y[id] = y;
        tp_motion(mem, id);
    }
}

/* The sticks as the game should see them: centred while the settings menu
 * has the controller. */
static HidAnalogStickState game_stick(int index) {
    HidAnalogStickState s = padGetStickPos(&g_pad, index);
#ifdef __SWITCH__
    if (menu_blocks_input()) {
        s.x = 0;
        s.y = 0;
    }
#endif
    return s;
}

static void tp_update(GuestMem *mem) {
    HidAnalogStickState l, r;
    if (!tp_engaged()) {
        if (g_tp_active[0]) { g_tp_active[0] = 0; tp_button(mem, 0, 0); }
        if (g_tp_active[1]) { g_tp_active[1] = 0; tp_button(mem, 1, 0); }
        return;
    }
    l = game_stick(0);
    r = game_stick(1);
    tp_stick(mem, 0, l.x, l.y, TP_W / 5,     TP_H / 2, TP_W / 5, TP_H / 2);
    tp_stick(mem, 1, r.x, r.y, TP_W * 4 / 5, TP_H / 2, g_tp_look_r, g_tp_look_ry);
}

/* ---- controller as touch ----------------------------------------------
 *
 * Both sticks become contacts, using the shape the capture measured rather
 * than a guessed one. The virtual sticks FLOAT -- they appear wherever a
 * finger lands -- so there are no fixed zones to hit, only an anchor to pick.
 *
 * Left is a stick: press at an anchor, hold the contact at anchor + deflection
 * scaled to PAD_MOVE_R. 95 px comes from the measurement -- sustained drags
 * clustered at 90-135 px from the anchor on a 480x320 screen.
 *
 * Right is NOT a stick. Aiming was repeated horizontal drags of 60-140 px over
 * 250-450 ms, so a held offset would turn once and then stop. It is a drag
 * that re-anchors: the contact walks in the deflection direction and, when it
 * nears an edge, lifts and starts again from the middle, which is what turning
 * continuously actually looks like to the game. */
#define PAD_DEADZONE  6000
#define PAD_MOVE_R    PX_W(170)
#define PAD_AIM_EDGE  PX_W(107)

/* The right stick has to work two ways, because the GAME has two schemes.
 *
 * In swipe-aim mode the right side of the screen is dragged to turn, so the
 * contact walks and re-anchors -- which is what it does below. In dual-stick
 * mode the right side is a held stick, and re-anchoring is actively wrong:
 * measured at 7 px a frame the contact reached the edge and lifted every
 * 283 ms, so the stick the game was drawing kept vanishing and reappearing.
 *
 * Live-settable rather than compiled in, because which is right depends on a
 * setting inside the game and the speed is a matter of feel. Guessing either
 * and rebuilding per attempt is how this kind of tuning gets abandoned
 * half-done. */
static int g_aim_stick = 0;      /* 0 = drag and re-anchor, 1 = held stick */
static int g_aim_speed = PX_W(53);  /* px per frame at full deflection */
static int g_aim_speed_y = PX_W(53); /* the same, vertically */
static int g_aim_radius = PX_W(170);/* held-stick deflection, as for the left */

static const int PAD_MOVE_AX = PX_W(226), PAD_MOVE_AY = PX_H(514);
static const int PAD_AIM_CX  = PX_W(887), PAD_AIM_CY  = PX_H(397);

static void pad_contact(GuestMem *mem, int slot, int want, int x, int y);

/* Touch down at the ANCHOR, then move.
 *
 * The game's virtual sticks float: whatever position a contact goes down at
 * becomes the stick's centre, and everything after is read as an offset from
 * it. Pressing straight to anchor+deflection therefore anchored the stick
 * wherever the thumb happened to be at that instant, so it landed somewhere
 * different every time and the on-screen control wandered around -- which is
 * exactly what it looked like.
 *
 * So the press is always at the anchor and the offset is applied from the next
 * frame on. One frame of delay, and the stick stays where it is put. */
static void pad_stick(GuestMem *mem, int slot, int ax, int ay, int dx, int dy) {
    Touch *t = &g_touch[slot];
    int first = !t->active;
    pad_contact(mem, slot, 1,
                clampi(first ? ax : ax + dx, 0, (int)SCREEN_W - 1),
                clampi(first ? ay : ay + dy, 0, (int)SCREEN_H - 1));
}

static void pad_contact(GuestMem *mem, int slot, int want, int x, int y) {
    Touch *t = &g_touch[slot];
    if (!want) {
        if (t->active) {
            touch_fire(mem, slot, 0);
            t->active = 0;
        }
        return;
    }
    t->raw_x = x;
    t->raw_y = y;
    if (!t->active) {
        t->active = 1;
        t->fid = 0xF000u + (uint32_t)slot;   /* cannot collide with a finger */
        t->x = x;
        t->y = y;
        touch_fire(mem, slot, 1);
    } else if (t->x != x || t->y != y) {
        t->x = x;
        t->y = y;
        touch_motion_fire(mem, slot);
    }
}

static void pad_touch_update(GuestMem *mem) {
    static int aim_x, aim_y, aim_down;
    HidAnalogStickState l, r;
    int lx, ly, rx, ry;
    padUpdate(&g_pad);
    tp_update(mem);
    l = game_stick(0);
    r = game_stick(1);
    lx = l.x < 0 ? -l.x : l.x;
    ly = l.y < 0 ? -l.y : l.y;
    rx = r.x < 0 ? -r.x : r.x;
    ry = r.y < 0 ? -r.y : r.y;

    /* With a real touchpad in play the sticks are delivered there instead;
     * doing both would move the player twice. The action tap below still
     * goes through as touch, because that IS a screen tap. */
    if (tp_engaged()) {
        pad_contact(mem, PAD_SLOT_MOVE, 0, 0, 0);
        pad_contact(mem, PAD_SLOT_AIM, 0, 0, 0);
        aim_down = 0;
        lx = ly = rx = ry = 0;
    }

    if (lx > PAD_DEADZONE || ly > PAD_DEADZONE) {
        /* Guest y is top-down, so pushing the stick up must decrease it. */
        pad_stick(mem, PAD_SLOT_MOVE, PAD_MOVE_AX, PAD_MOVE_AY,
                  l.x * PAD_MOVE_R / 32767, -l.y * PAD_MOVE_R / 32767);
    } else {
        pad_contact(mem, PAD_SLOT_MOVE, 0, 0, 0);
    }

    /* The action tap, a few frames long so the game sees a real contact
     * rather than a single-frame blip. */
    if (g_act_frames > 0) {
        g_act_frames--;
        pad_contact(mem, PAD_SLOT_TAP, 1,
                    clampi(g_act_x, 0, (int)SCREEN_W - 1),
                    clampi(g_act_y, 0, (int)SCREEN_H - 1));
    } else {
        pad_contact(mem, PAD_SLOT_TAP, 0, 0, 0);
    }

    if ((rx > PAD_DEADZONE || ry > PAD_DEADZONE) && g_aim_stick) {
        /* Dual-stick: a floating stick, exactly like the left one. */
        aim_down = 1;
        pad_stick(mem, PAD_SLOT_AIM, PAD_AIM_CX, PAD_AIM_CY,
                  r.x * g_aim_radius / 32767, -r.y * g_aim_radius / 32767);
    } else if (rx > PAD_DEADZONE || ry > PAD_DEADZONE) {
        if (!aim_down) {
            aim_x = PAD_AIM_CX;
            aim_y = PAD_AIM_CY;
            aim_down = 1;
        }
        aim_x += r.x * g_aim_speed / 32767;
        aim_y -= r.y * g_aim_speed_y / 32767;
        /* Off the edge: lift and start again from the middle, so a held stick
         * keeps turning instead of stopping at the screen border. */
        if (aim_x < PAD_AIM_EDGE || aim_x > (int)SCREEN_W - PAD_AIM_EDGE ||
            aim_y < PAD_AIM_EDGE || aim_y > (int)SCREEN_H - PAD_AIM_EDGE) {
            pad_contact(mem, PAD_SLOT_AIM, 0, 0, 0);
            aim_x = PAD_AIM_CX;
            aim_y = PAD_AIM_CY;
        }
        pad_contact(mem, PAD_SLOT_AIM, 1, aim_x, aim_y);
    } else if (aim_down) {
        pad_contact(mem, PAD_SLOT_AIM, 0, 0, 0);
        aim_down = 0;
    }
}

static void touch_update(GuestMem *mem) {
    HidTouchScreenState ts;
    Touch now[REAL_TOUCH];
    int i, j, n = 0;

    memset(now, 0, sizeof now);
    if (g_touch_ready &&
#ifdef __SWITCH__
        !menu_blocks_input() &&
#endif
        hidGetTouchScreenStates(&ts, 1)) {
        for (i = 0; i < (int)ts.count && n < REAL_TOUCH; i++) {
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
                g_tap_y = now[0].y;
                /* Keep the stick cursor parked on the same contact, so the
                 * idle path's g_tap := cursor cannot drag the getters back to
                 * an older touch when input_poll skipped this one (bezel). */
                g_cur_x = now[0].x;
                g_cur_y = now[0].y;
            }
            n++;
        }
    }

    /* Releases first: a slot whose finger id is no longer present. */
    for (i = 0; i < REAL_TOUCH; i++) {
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
        for (i = 0; i < REAL_TOUCH; i++)
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
        for (i = 0; i < REAL_TOUCH && slot < 0; i++)
            if (!g_touch[i].active)
                slot = i;
        if (slot < 0)
            continue;                   /* more fingers than slots; ignore */
        g_touch[slot] = now[j];
        touch_fire(mem, slot, 1);
    }
}

/* Sequence number shared by Update and the getters, so the log shows the
 * ORDER. s3ePointerUpdate is a GUEST call: if the game reads the getters
 * before calling it, it is reading the previous sample by construction, and
 * no amount of correcting the coordinates can help. */
static unsigned g_ptr_seq;

static void hle_ptr_update_body(GuestCpu *cpu, GuestMem *mem, void *user);

static void hle_ptr_update(GuestCpu *cpu, GuestMem *mem, void *user) {
    static int shown;
    hle_ptr_update_body(cpu, mem, user);
    g_ptr_seq++;
    if (g_touch_dbg && shown < 200) {
        shown++;
        printf("  [upd  ] seq=%u tap=(%d,%d) state=%d down=%d\n",
               g_ptr_seq, g_tap_x, g_tap_y, g_ptr_state, g_real_down);
    }
}

static void hle_ptr_update_body(GuestCpu *cpu, GuestMem *mem, void *user) {
    static int last_motion_x = -1, last_motion_y = -1;
    int x = 0, y = 0, down;
    (void)user;

    down = input_poll(&x, &y);
    touch_update(mem);
    pad_touch_update(mem);   /* the sticks, as two more contacts */
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
    /* Position leads the button by one update.
     *
     * Everything on our side measures correct: the overlay squares coincide,
     * press and release carry their own coordinates (14/17, the misses being
     * drags), and GetX returns the new position on the very update the press
     * is reported. The game still acts on the previous position, which means
     * it tests the button BEFORE it consumes the coordinate -- it acts on a
     * cursor it latched on an earlier update. That is why pressing twice
     * works: the first press moves its cursor, the second acts on it.
     *
     * A real touchscreen hides this, because contact position is reported a
     * little before the pressed state settles. Here both arrive in the same
     * update. So the first update of a contact delivers the position and the
     * motion event with the button still up, and the press is reported on the
     * next one -- which is exactly the two-press dance done for the game
     * instead of by the user.
     *
     * A contact that vanishes before that second update still has to produce a
     * press, or fast taps would be swallowed entirely. */
    static int press_armed;
    int report_down = down;
    if (down && !g_real_down) {
        if (!press_armed) {
            press_armed = 1;
            report_down = 0;        /* this update carries position only */
            /* The block below is skipped when down is 0, so publish the
             * position here or the "position only" update carries nothing.
             * touch_update happens to have set g_tap from the contact already,
             * but the stick cursor has no such side effect, and the guest's
             * hover path (RVA 0x81908: obj+0x60 := widget under the pointer,
             * only while NOT touching) is the whole point of this update. */
            g_tap_x = x;
            g_tap_y = y;
            mark_update(1, g_touch[0].raw_x, g_touch[0].raw_y, x, y);
            if (x != last_motion_x || y != last_motion_y) {
                motion_fire(mem, x, y);
                last_motion_x = x;
                last_motion_y = y;
            }
        } else {
            press_armed = 0;
        }
    } else if (!down) {
        if (press_armed) {
            press_armed = 0;
            report_down = 1;        /* too quick to split: press now, release next */
        }
    }
    down = report_down;
    if (down || g_real_down) {
        /* Only while a finger is down.
         *
         * On the release frame g_touch[0] has been cleared, so the override
         * above does not run and x,y come from input_poll's stick-cursor
         * fallback instead of the tracked contact. Assigning from that pushed
         * a coordinate from an unrelated source into the RELEASED event and
         * into the polled getters -- and while the Y flip existed it was the
         * MIRRORED one, so a menu press landed on the right widget and its
         * release landed on the widget's vertical mirror. UI buttons act on
         * release, which is precisely why menus read as reversed while
         * gameplay, driven by ids 2/3, was correct.
         *
         * Removing the flip hides this by making both sources agree. It is
         * still wrong: the SDK contract is that the coordinates stay at the
         * last tap location after release. */
        if (down) {
            g_tap_x = x;
            g_tap_y = y;
        }
        /* Marker takes the screen-space y, not the UI-space one: when the
         * conversion is right the game acts where the finger is, so the red
         * square should sit on the green one. */
        mark_update(down, g_touch[0].raw_x, g_touch[0].raw_y, x, y);
        if (!g_saw_real_input && !g_bench) {
            g_saw_real_input = 1;
            printf("  [in   ] real input active -- synthetic tap stands down\n");
        }
        /* Position BEFORE button, which is the whole bug.
         *
         * The game's menu keeps a highlighted item, moves it from the motion
         * event, and acts on the press event. Queueing the press first meant a
         * fresh touch clicked whatever was highlighted from the PREVIOUS touch
         * and only then moved the highlight -- so the first press moved the
         * cursor, the second applied it, and when the stale highlight happened
         * to be on a clickable item the first press activated that instead.
         * That is the "I press once to move, once to apply" behaviour, and why
         * it felt arbitrary: it depended on what was under the old position.
         *
         * A real device reports the contact's position before its button
         * transition, so delivering motion first is also what the SDK contract
         * implies. */
        if (down && (x != last_motion_x || y != last_motion_y)) {
            motion_fire(mem, x, y);
            last_motion_x = x;
            last_motion_y = y;
        }

        /* id 1 is s3ePointerMotionEvent {x,y}. The old port only updated the
         * polling getters, so code driven by the registered motion callback
         * saw the press at one position but no drag/movement at all. */
        if (down && !g_real_down) {
            g_ptr_state = S3E_PTR_PRESSED;
            /* Ground truth for the coordinate path: the raw panel sample, the
             * tracked contact, and what the polled getters will report.
             * Guessing at this has produced two wrong fixes already. */
            if (g_touch_dbg)
                printf("  [tapdbg] PRESS panel=(%d,%d) touch0=(%d,%d) tap=(%d,%d)\n",
                   g_touch[0].raw_x, g_touch[0].raw_y,
                   g_touch[0].x, g_touch[0].y, g_tap_x, g_tap_y);
            tap_fire(mem, &g_ev_press, 1);
        } else if (down) {
            g_ptr_state = S3E_PTR_DOWN;
        } else {
            g_ptr_state = S3E_PTR_RELEASED;
            if (g_touch_dbg)
                printf("  [tapdbg] RELEASE tap=(%d,%d) xy=(%d,%d)\n",
                   g_tap_x, g_tap_y, x, y);
            tap_fire(mem, &g_ev_release, 0);
        }
        g_real_down = down;
        cpu->r[0] = 0;
        return;
    }
    g_real_down = 0;
    if (g_saw_real_input) {
        g_ptr_state = S3E_PTR_UP;
        /* Button up: the getters follow the cursor, as a mouse's would. The
         * guest only moves its hover widget (obj+0x60, RVA 0x81908) while the
         * pointer is up, and it sends the press to THAT widget (RVA 0x819c6),
         * so a docked stick cursor that is invisible until A is pressed hits
         * whatever was hovered on the previous press. In handheld mode this is
         * a no-op: input_poll leaves g_cur at the last contact. */
        g_tap_x = x;
        g_tap_y = y;
    }

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
    if (g_touch_dbg && shown < 200) {
        shown++;
        printf("  [read ] seq=%u %s -> %u  (caller RVA %06x)\n",
               g_ptr_seq, what, (unsigned)v, (unsigned)lr);
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

/* A sink for stdout once the console is gone.
 *
 * consoleExit() tears the console down, and libnx leaves its device installed
 * on STD_OUT -- so the next printf stores into a buffer that is no longer
 * there. That is a NULL write, and it is exactly where the game died when
 * launched with no PC to log to: native_stage 123, one statement after
 * win_release(), on the printf announcing the handover.
 *
 * It never happened with nxlink connected because nxlinkStdio replaces the
 * STD_OUT device with its socket, so the console is not in the path at all.
 * The offline case is the only one that writes to a dead console, which is
 * why "works here, crashes for you" was the shape of it.
 *
 * Swapping the DEVICE rather than the FILE matters: stdout is per-thread in
 * newlib, but the mixer and control threads print too, and they all resolve
 * through this one table. */
static int g_nxlink_up;

static ssize_t null_write(struct _reent *r, void *fd, const char *p, size_t n) {
    (void)r; (void)fd; (void)p;
    return (ssize_t)n;
}

static const devoptab_t g_dev_null = {
    .name = "boznull", .structSize = 0, .write_r = null_write
};

static void log_drop_console(void) {
    if (g_nxlink_up)            /* nxlink owns STD_OUT; leave it alone */
        return;
    devoptab_list[STD_OUT] = &g_dev_null;
    devoptab_list[STD_ERR] = &g_dev_null;
}


static void win_release(void) {
    switch (g_win) {
    case WIN_CONSOLE:
        g_native_stage = 121;
        log_drop_console();   /* before the buffer goes away */
        consoleExit(NULL);
        g_native_stage = 122;
        break;
    case WIN_FB:      framebufferClose(&g_fb);  break;
    case WIN_EGL:     break;   /* only EGL's own teardown releases its buffers */
    default:          break;
    }
    g_win = WIN_NONE;
}

/* ------------------------------------------------------------ frame profile
 *
 * Splits a frame into ticks spent inside HLE handlers (GL, EGL, file,
 * everything reached through the stub page) versus ticks spent running guest
 * code, plus the worst individual imports by name. Everything is 19.2 MHz
 * system ticks. */
static uint64_t g_prof_hle_ticks;         /* inside handlers, this window */
static uint64_t g_prof_enter;             /* tick at the current handler entry */
static uint32_t g_prof_depth;             /* handlers can re-enter via callbacks */
static uint64_t g_prof_slot_ticks[512];
static uint32_t g_prof_slot_calls[512];
static uint32_t g_prof_slot;

/* The guest clock itself lives with hle_timer_ms far below; these two are
 * up here because the frame report prints how often a frame the game asks
 * the time, which is the number that made the clock a bug, not a detail. */
static uint32_t g_clock_queries;
static int g_clock_fixed;

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

/* Set with g.prof, from the profilers setting: is the handler split measured? */
static int g_prof_on;

/* Scheduling of the guest thread itself; filled in at startup. */
static int32_t g_main_prio = -1;

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
        g_clock_queries = 0;
        return;
    }
    total = now - window_start;
    hle = g_prof_hle_ticks;
    if (!total)
        return;

    if (!g_prof_on) {
        /* Without the profilers nothing is timed, but the call counts below
         * are free: the dispatcher increments them either way. */
        printf("  [prof ] %llu ms/300f (turn Profilers on in the menu's"
               " Advanced tab for the handler split)\n",
               (unsigned long long)(total * 1000ull / freq));
    } else {
        printf("  [prof ] %llu ms/300f: handlers %llu%%, guest code %llu%%\n",
               (unsigned long long)(total * 1000ull / freq),
               (unsigned long long)(hle * 100ull / total),
               (unsigned long long)((total - (hle < total ? hle : total)) * 100ull
                                    / total));
    }

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

    /* And the same ranking by CALL COUNT, which needs no timing and is the
     * one that shows where the boundary is being crossed. A handler can be
     * 0% of the time and still be called 800 times a frame -- that is 800
     * register syncs and dispatcher round trips, and it is invisible in a
     * ranking by ticks. The counts are collected by the dispatcher whether or
     * not the profiler is on, so this prints in every build. */
    {
        unsigned calls_total = 0;
        int shown, taken[10];
        for (i = 0; i < 512; i++)
            calls_total += g_prof_slot_calls[i];
        printf("  [calls] %u HLE calls/300f = %u per frame\n",
               calls_total, calls_total / 300u);
        for (shown = 0; shown < 10; shown++) {
            unsigned bestc = 0;
            int k;
            taken[shown] = -1;
            for (i = 0; i < 512; i++) {
                int seen = 0;
                for (k = 0; k < shown; k++)
                    if (taken[k] == i) seen = 1;
                if (!seen && g_prof_slot_calls[i] > bestc) {
                    bestc = g_prof_slot_calls[i];
                    taken[shown] = i;
                }
            }
            if (taken[shown] < 0 || !bestc)
                break;
            printf("  [calls]   %-28s %6u/frame  %2u%%\n",
                   slot_name((uint32_t)taken[shown]), bestc / 300u,
                   calls_total ? (unsigned)((uint64_t)bestc * 100ull / calls_total) : 0u);
        }
    }

    /* Queries per frame is the whole diagnosis of the clock bug: at one per
     * frame the fixed step ran slow, above about three it ran fast. Printed
     * under either clock, so a fixed-step run can still be characterised. */
    printf("  [clock] %s, %u queries/300f = %llu.%llu per frame\n",
           g_clock_fixed ? "fixed 16 ms/query" : "real time",
           (unsigned)g_clock_queries,
           (unsigned long long)(g_clock_queries / 300u),
           (unsigned long long)((g_clock_queries % 300u) * 10u / 300u));
    g_clock_queries = 0;

    memset(g_prof_slot_ticks, 0, sizeof g_prof_slot_ticks);
    memset(g_prof_slot_calls, 0, sizeof g_prof_slot_calls);
    g_prof_hle_ticks = 0;
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
    window_start = now;
}

/* ------------------------------------------- instruction-anchored window
 *
 * frame_profile_report divides by 300 PRESENTS, which makes its ms/300f
 * incomparable between two runs: the same guest work lands at different
 * present counts, so window 1 of one run and window 1 of another can hold
 * 890M and 305M instructions and be reported side by side as though they
 * were the same experiment. That is exactly how a 4% difference got
 * quoted from a pair of windows that shared nothing.
 *
 * Guest instructions are the honest axis. A window of a fixed number of
 * them covers the same work in every run, so M instr/s can be compared
 * directly -- across builds, across card-flag settings, across sessions.
 * Read that, not fps, when asking whether a change made the CPU faster;
 * fps still answers whether the frame got faster, which is a different
 * question whenever the work per frame is not fixed. */
#define INSTR_WINDOW 500000000ull

static uint64_t g_iw_next;      /* executed count at the next report */
static uint64_t g_iw_tick;      /* tick at this window's start */
static uint64_t g_iw_instr;     /* executed count at this window's start */
static int      g_iw_presents;  /* presents at this window's start */

static void instr_profile_report(uint64_t executed, int presents) {
    uint64_t now = armGetSystemTick(), freq = armGetSystemTickFreq();
    uint64_t ms, di, r10;

    if (!g_iw_tick) {           /* first call sets the origin */
        g_iw_tick = now;
        g_iw_instr = executed;
        g_iw_presents = presents;
        g_iw_next = executed + INSTR_WINDOW;
        return;
    }
    if (executed < g_iw_next)
        return;

    ms = (now - g_iw_tick) * 1000ull / freq;
    di = executed - g_iw_instr;
    r10 = ms ? di / (ms * 100ull) : 0;      /* M instr/s, x10 */
    printf("  [iwin ] %lluM instructions in %llu ms = %llu.%llu M/s,"
           " %d presents\n",
           (unsigned long long)(di / 1000000ull), (unsigned long long)ms,
           (unsigned long long)(r10 / 10ull), (unsigned long long)(r10 % 10ull),
           presents - g_iw_presents);

    g_iw_tick = now;
    g_iw_instr = executed;
    g_iw_presents = presents;
    g_iw_next = executed + INSTR_WINDOW;
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
static uint32_t g_xmap[VIEW_W];

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

/* 3x5 glyphs, the same encoding fps_overlay uses in gl_egl.c. Duplicated
 * rather than shared because that one draws with glScissor and glClear
 * into an EGL surface, and this one pokes pixels into a libnx Framebuffer;
 * only the bitmaps are common, and they are five lines. */
static const char *fb_glyph(char ch) {
    switch (ch) {
    case 'F': return "111100110100100";
    case 'P': return "110101110100100";
    case 'S': return "111100111001111";
    case '0': return "111101101101111";
    case '1': return "010110010010111";
    case '2': return "111001111100111";
    case '3': return "111001111001111";
    case '4': return "101101111001001";
    case '5': return "111100111001111";
    case '6': return "111100111101111";
    case '7': return "111001001001001";
    case '8': return "111101111101111";
    case '9': return "111101111001111";
    default:  return "000000000000000";
    }
}

/* The GL path has had an FPS counter all along; this is the same thing for
 * the software framebuffer, which is what presents during loading and any
 * phase before EGL takes the window. Without it the counter vanishes for
 * exactly the stretches that are slowest and most worth watching.
 *
 * Counts its own frames rather than sharing the GL counter: the two paths
 * never present at the same time, and a shared tally would read as a
 * collapse to zero every time the window changed hands. */
static void fb_fps_overlay(uint8_t *out, uint32_t stride) {
    static uint64_t last_tick;
    static uint32_t frames, shown;
    uint64_t now = armGetSystemTick(), freq = armGetSystemTickFreq(), el;
    char text[16];
    int scale = 3, x0 = 10, y0 = 10, ci, row, col, sx, sy;

    if (!last_tick)
        last_tick = now;
    frames++;
    el = now - last_tick;
    if (el >= freq) {
        shown = (uint32_t)(((uint64_t)frames * freq + el / 2u) / el);
        frames = 0;
        last_tick = now;
    }
    snprintf(text, sizeof text, "FPS %u", (unsigned)shown);
    for (ci = 0; text[ci]; ci++) {
        const char *bits = fb_glyph(text[ci]);
        for (row = 0; row < 5; row++)
            for (col = 0; col < 3; col++) {
                /* Black for the unset pixels too, so the digits keep a
                 * solid backing box and stay readable over bright art. */
                uint32_t c = (bits[row * 3 + col] == '1')
                           ? 0xFF00FF00u : 0xFF000000u;
                for (sy = 0; sy < scale; sy++)
                    for (sx = 0; sx < scale; sx++) {
                        uint32_t px = (uint32_t)(x0 + ci * 4 * scale
                                                 + col * scale + sx);
                        uint32_t py = (uint32_t)(y0 + row * scale + sy);
                        if (px < FB_W && py < FB_H)
                            *((uint32_t *)(out + (size_t)py * stride) + px) = c;
                    }
            }
    }
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
    fb_fps_overlay(out, stride);
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

/* The guest clock.
 *
 * This was a virtual clock: 16 ms added PER QUERY, matching the Unicorn
 * reference exactly so a run stayed reproducible. The constant-0 stub it
 * replaced was worse than either, because any "wait until N ms have elapsed"
 * loop then never terminates -- that is what pinned the run in a spin from 10M
 * to 50M instructions.
 *
 * But per query is not per frame. The game asks the time several times a frame,
 * so the clock advanced several times 16 ms per frame and ran AHEAD of real
 * time: game speed = 16 ms x queries-per-frame x fps / 1000. That is why
 * zombies and NPCs moved too fast at 21 fps, and it is linear in frame rate --
 * every optimisation made the game run faster rather than smoother, so 30 fps
 * would have been 1.4x as fast again and 60 fps 2.8x. The natural guess, that a
 * fixed step means slow motion, is only true at exactly one query per frame.
 *
 * Real elapsed time removes the coupling: the game plays at one speed whatever
 * the frame rate. Two queries in the same millisecond now return the same
 * value, which the fixed step never did -- but that is exactly what the game
 * saw on the hardware it shipped on, so its own dt handling already covers it.
 *
 * Every query reads the tick afresh rather than returning a value latched once
 * per frame, so an in-frame wait loop still terminates. The old counter stays
 * one setting away (fixed_clock) because reproducibility is what a comparison
 * against the Unicorn reference needs; hostdiff keeps its own copy regardless. */
static uint32_t g_ticks;              /* the fixed-step fallback's counter */
static uint64_t g_clock_base;         /* tick at the first query */

static void hle_timer_ms(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    g_clock_queries++;
    /* s3eTimerGetMs and s3eTimerGetUST return uint64 in r0:r1. Only r0 was
     * ever set, so the high half was whatever r1 held: harmless where the game
     * keeps the low word, but Demonware's socket wrapper (RVA 0x16a30) stores
     * all 64 bits as a connection's start time and times the connection out
     * against it. */
    cpu->r[1] = 0;
    if (g_clock_fixed) {
        g_ticks += 16;
        cpu->r[0] = g_ticks;
        return;
    }
    {
        uint64_t now = armGetSystemTick();
        if (!g_clock_base)
            g_clock_base = now;
        cpu->r[0] = (uint32_t)(((now - g_clock_base) * 1000ull)
                               / armGetSystemTickFreq());
    }
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
/* Trace the audio imports with their arguments.
 *
 * hle_sound_channel_play, _stop, _getfreechannel and hle_audio_ok were
 * completely silent, so a session could drive the whole sound API and leave no
 * evidence of it. That is what made "did this game play anything" unanswerable
 * from a log -- and the stub call counts cannot answer it either, because that
 * counter lives in hle_default and only ever sees imports with NO handler.
 *
 * The arguments matter more than the count. s3eSoundChannelPlay carries a PCM
 * buffer and a sample count, while s3eAudioPlay carries a filename pointer;
 * which of those the game uses is the difference between wiring an audout sink
 * to buffers the game already mixed, and having to decode a stream. Four calls
 * per import is enough to see the shape and cheap enough to leave on. */
#define SND_CHANNELS 16

static uint8_t g_snd_traced[512];

static void snd_trace(void *user, const GuestCpu *cpu) {
    uint32_t idx = (uint32_t)(uintptr_t)user;
    if (idx >= 512 || g_snd_traced[idx] >= 40)
        return;
    g_snd_traced[idx]++;
    /* lr too: the game never calls s3eAudioPlay, so the question for music is
     * which guest code decides that -- and the only audio entry points it does
     * call (Resume, Stop, Pause) name their caller in lr. Disassembling around
     * that address shows the condition, where guessing at what a config key
     * named auEnabled means does not. */
    printf("  [snd  ] %-26s r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x\n",
           slot_name(idx), (unsigned)cpu->r[0], (unsigned)cpu->r[1],
           (unsigned)cpu->r[2], (unsigned)cpu->r[3],
           (unsigned)cpu->r[GUEST_LR]);
}

static uint8_t  g_snd_playing[SND_CHANNELS];
static uint8_t  g_snd_paused[SND_CHANNELS];   /* s3eSoundChannelPause; GetInt 5 */
static uint8_t  g_snd_wedged[SND_CHANNELS];   /* yields seen playing-but-silent */

/* Per-channel state. Recorded now for the trace, but this is the state a mixer
 * has to keep regardless, so it is kept properly rather than printed and
 * thrown away.
 *
 * s3eSoundChannelSetInt was bound to hle_audio_ok -- a bare "return success"
 * -- so everything the game configured was being discarded, including the
 * 22050 Hz it sets on every channel before playing. Nothing could have been
 * mixed from that. */
typedef struct {
    uint32_t rate;              /* Hz, as set by the game */
    uint32_t buf;               /* guest PCM pointer from Play */
    uint32_t samples;           /* count from Play */
    uint32_t repeat;
    uint32_t prop[8];           /* whatever else it sets, by id */
    uint32_t prop_seen;
    uint8_t  desc_fresh;        /* property 2 set since the last Play */
    uint8_t  was_adpcm;         /* how the current sound was started, for a repeat */
} SndChannel;

static SndChannel g_snd[SND_CHANNELS];

/* Whether real output came up. When it did, the mixer is the authority on
 * which voices are still sounding; when it did not, the old bookkeeping stands
 * so the game still sees a plausible device. */
static int g_snd_live;
static uint8_t g_scratch[SCRATCH_SIZE];

/* One info block per channel, so one callback cannot scribble on another's.
 * 32 bytes is well clear of the +12 the handler touches. */
#define ENDINFO_STRIDE 32u
#define ENDINFO_ADDR(ch) (SCRATCH_BASE + (uint32_t)(ch) * ENDINFO_STRIDE)
static unsigned g_snd_free_shown;
static unsigned g_snd_end_shown;
static uint32_t g_endinfo_pending;

/* Whether a drained voice fires the game's end-of-sample callback. ON: the
 * game cannot repeat a sound without it.
 *
 * This was off for a while, on the strength of an A/B that I misread. Turning
 * it off mid-session appeared to stop a looping barricade sound -- but an NPC
 * started firing at that instant and would have taken the channel regardless,
 * which I noticed at the time and reasoned past. I then checked only that
 * gunfire still worked, and gunfire works either way because every shot is a
 * fresh request.
 *
 * What actually needs it is anything that plays the SAME sound again: menu
 * clicks, footsteps, the knife. The game never polls channel status, so this
 * notification is the only thing that tells it a sound finished; without it
 * the first click plays and nothing is ever requested again. Measured with the
 * toggle in a live session:
 *
 *     Play 04a0e320 -> firing end-of-sample -> Play 04a0e320 -> ...
 *
 * four times over, which is the menu working. With it off, one play and
 * silence for the rest of the session. */
static int g_snd_endcb = 1;

/* Whether the s3eAudio imports do anything, or behave as they did before music
 * existed -- report success and nothing else.
 *
 * Menu taps used to make a sound on every tap and now make one only on the
 * first, and the music work is the only thing that changed between those two
 * builds. The mixer is provably fine (it fills buffers at exactly 47 a second
 * with voices draining), so the suspect is these handlers: six of them changed
 * at once, and this switches all six back without a rebuild rather than
 * bisecting by guesswork. */
static int g_audio_hle = 1;
static unsigned g_snd_stat_shown;

/* Tell the game which sounds have finished.
 *
 * It registered two s3eSoundChannel callbacks and then never polled our
 * channel status once -- so it tracks what is playing from its own
 * bookkeeping, and that bookkeeping only advances when a callback fires.
 * Without this it plays one sound, waits forever for the end of it, and goes
 * quiet: which is exactly what happened.
 *
 * id 0 of the two registered is taken to be end-of-sample. If the game stays
 * silent, the other is the next thing to try, and the log says which fired.
 * Called from the yield handler rather than the mixer because the callback
 * queue belongs to the guest thread. */
/* What the end-of-sample handler stored into the info block. Read after the
 * callback pump, which is where it actually runs. The layout is not documented
 * anywhere we can consult, so it is reported rather than assumed: +8 and +12
 * are the two the handler writes, and they are how the game asks for a sound
 * to continue. */
static void snd_endinfo_readback(void) {
    static unsigned shown;
    uint32_t a = 0, b = 0, c = 0;
    if (!g_endinfo_pending || !g_memp)
        return;
    guest_ld32(g_memp, g_endinfo_pending + 4u, &a);
    guest_ld32(g_memp, g_endinfo_pending + 8u, &b);
    guest_ld32(g_memp, g_endinfo_pending + 12u, &c);
    if (shown < 12u && (a || b || c)) {
        shown++;
        printf("  [snd  ] end-info wrote +4=%08x +8=%08x +12=%08x\n",
               (unsigned)a, (unsigned)b, (unsigned)c);
    }
    g_endinfo_pending = 0;
}

static void snd_pump_finished(void) {
    uint32_t done = snd_out_take_drained();
    unsigned ch;
    /* Taken unconditionally: leaving the mask standing while the callback is
     * off would fire a burst of stale ones the moment it is switched back. */
    if (!g_snd_endcb) {
        /* STATUS reads g_snd_playing, so a drained voice must still clear it. */
        for (ch = 0; ch < SND_CHANNELS; ch++)
            if (done & (1u << ch))
                g_snd_playing[ch] = 0;
        return;
    }
    for (ch = 0; ch < SND_CHANNELS && done; ch++) {
        if (!(done & (1u << ch)))
            continue;
        done &= ~(1u << ch);
        if (!g_snd_playing[ch])
            continue;              /* not the game's voice (the selftest tone) */
        g_snd_playing[ch] = 0;
        if (g_snd_end_shown < 20) {
            g_snd_end_shown++;
            printf("  [snd  ] ch%u finished, firing end-of-sample\n", ch);
        }
        /* A real, writable address rather than the channel number. The
         * handler stores through this, and what it leaves at +8 and +12 is the
         * game saying what to play next -- which is how a repeating or
         * streamed sound continues. Reported rather than guessed at. */
        if (!g_memp)
            continue;
        snd_finish_channel(ch);
    }
}

/* STATUS follows g_snd_playing, not the mixer. The mixer drains a voice on its
 * own thread, but the end-of-sample decision (and a looping sound's restart)
 * only happens at the next yield. Answering from the mixer let the game poll
 * STATUS inside that gap, see 0, and retire the sound instance without ever
 * calling Stop -- after which the restart kept the loop going with nobody left
 * to fade it or stop it. That was the endless teleporter and barricade-repair
 * sounds. Real Marmalade loops inside the mixer, so there is no such gap.
 * g_snd_playing is cleared only by Stop or by an end-of-sample that stops. */
/* A channel leaves "playing" through a drain, a Stop, or an end-of-sample that
 * says stop. If the mixer is live and holds no voice for a channel that is not
 * paused, none of those will ever happen: the play started nothing, or its
 * voice was replaced. Retire it after a few yields, so the game can have the
 * channel back. Without this, one such channel is lost for the whole run. */
static void snd_pump_wedged(void) {
    static unsigned wedge_log;
    unsigned ch;
    if (!g_snd_live)
        return;
    for (ch = 0; ch < SND_CHANNELS; ch++) {
        if (!g_snd_playing[ch] || g_snd_paused[ch] || snd_out_busy(ch)) {
            g_snd_wedged[ch] = 0;
            continue;
        }
        if (++g_snd_wedged[ch] < 3u)
            continue;
        g_snd_wedged[ch] = 0;
        g_snd_playing[ch] = 0;
        if (wedge_log < 50u) {
            wedge_log++;
            printf("  [snd  ] ch%u had no voice: freeing it\n", (unsigned)ch);
        }
    }
}

/* A voice with most of its audio left, ended by something other than reaching
 * its end, is a sound the player hears cut off. Reported with what did it. */
static void snd_report_cut(uint32_t ch, const char *why) {
    static unsigned cut_log;
    uint32_t left = 0, total = 0;
    if (ch >= SND_CHANNELS)
        return;
    snd_out_progress(ch, &left, &total);
    if (!total || left * 4u < total)          /* past three quarters: not a cut */
        return;
    if (cut_log < 2000u) {
        cut_log++;
        printf("  [snd  ] ch%u CUT with %u of %u samples left: %s\n",
               (unsigned)ch, (unsigned)left, (unsigned)total, why);
    }
}

static int snd_channel_busy(uint32_t ch) {
    if (ch >= SND_CHANNELS)
        return 0;
    return (int)g_snd_playing[ch];
}

static void logprop(const char *who, uint32_t prop, uint32_t v,
                    uint32_t *seen) {
    if (prop < 32 && !(*seen & (1u << prop))) {
        *seen |= 1u << prop;
        printf("  [snd  ] %s(%u) -> %u\n", who, (unsigned)prop, (unsigned)v);
    }
}

/* s3eSoundGetInt: 0 is S3E_SOUND_NUM_CHANNELS in every Marmalade build I can
 * check against, and answering 0 there is what leaves the mixer unbuilt. */
/* s3eSoundGetInt / SetInt, numbered as Marmalade -- and as the PortMaster
 * reference, which plays every sound in this game -- number them:
 *   0 VOLUME (0..256)  1 DEFAULT_FREQ  2 OUTPUT_FREQ  3 NUM_CHANNELS
 *   5 AVAILABLE        7 STEREO        anything else -1
 *
 * This used to answer 0 with the channel count and 3 with 1. The game copies
 * property 3 into its sound manager's channel limit and rejects any free
 * channel at or above it (RVA 0xd75e6), so it could only ever use channel 0:
 * a sound that started while another was playing was simply dropped. And
 * property 0 told it the master volume was 16 of 256. */
static uint32_t g_snd_volume = 256u;
static uint32_t g_snd_rate = 22050u;

static void hle_sound_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0], v;
    (void)mem; (void)user;
    switch (prop) {
    case 0:  v = g_snd_volume;  break;     /* VOLUME       */
    case 1:  v = 22050u;        break;     /* DEFAULT_FREQ */
    case 2:  v = g_snd_rate;    break;     /* OUTPUT_FREQ  */
    case 3:  v = SND_CHANNELS;  break;     /* NUM_CHANNELS */
    case 5:  v = 1;             break;     /* AVAILABLE    */
    case 7:  v = 1;             break;     /* STEREO       */
    default: v = 0xFFFFFFFFu;   break;
    }
    logprop("SoundGetInt", prop, v, &seen);
    cpu->r[0] = v;
}

static void hle_sound_setint(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t prop = cpu->r[0], val = cpu->r[1];
    (void)mem; (void)user;
    if (prop == 0u) {                       /* VOLUME: master for every sample */
        g_snd_volume = (int32_t)val < 0 ? 0u : (val > 256u ? 256u : val);
        snd_out_set_master(g_snd_volume);
        cpu->r[0] = 0;
    } else if (prop == 2u) {
        g_snd_rate = val;
        cpu->r[0] = 0;
    } else {
        cpu->r[0] = 1;                      /* S3E_RESULT_ERROR */
    }
}

/* s3eAudioGetInt / SetInt, Marmalade numbering (as the reference port):
 *   0 VOLUME (0..256)  1 STATUS (0 stopped, 1 playing, 2 paused)
 *   4 CHANNEL (selected stream)  5 NUM_CHANNELS  6, 9 AVAILABLE  else -1
 *
 * The music manager polls STATUS every frame and wipes its state -- a queued
 * track included -- the moment it reads 0 (RVA 0x1b9ea4), so a PAUSED stream
 * has to say 2, not 0. There is one stream here, so only channel 0 selects. */
static uint32_t g_audio_volume = 256u;

static void hle_audio_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0], v;
    (void)mem; (void)user;
    switch (prop) {
    case 0:  v = g_audio_volume; break;
    case 1:  v = g_audio_hle ? (uint32_t)snd_music_status() : 0u; break;
    case 4:  v = 0;  break;
    case 5:  v = 1;  break;
    case 6:
    case 9:  v = 1;  break;
    default: v = 0xFFFFFFFFu; break;
    }
    logprop("AudioGetInt", prop, v, &seen);
    cpu->r[0] = v;
}

static void hle_audio_setint(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t prop = cpu->r[0], val = cpu->r[1];
    (void)mem; (void)user;
    if (prop == 0u) {                       /* VOLUME */
        g_audio_volume = (int32_t)val < 0 ? 0u : (val > 256u ? 256u : val);
        snd_music_set_volume(g_audio_volume);
        cpu->r[0] = 0;
    } else if (prop == 4u) {                /* CHANNEL: only stream 0 exists */
        cpu->r[0] = val == 0u ? 0u : 1u;
    } else {
        cpu->r[0] = 1;                      /* S3E_RESULT_ERROR */
    }
}

/* s3eAudioPlay(filename, repeatCount). The game names its tracks with paths
 * relative to the asset root -- blackops-music/mus_gameover.mp3 -- which is
 * where they now live on the card.
 *
 * repeatCount 0 is taken as looping: it is what the sample API uses for the
 * same thing, and a track asked to play zero times is not a sensible request.
 * The log says which it chose, so a wrong reading here is audible AND
 * visible rather than merely puzzling. */
static void hle_audio_play(GuestCpu *cpu, GuestMem *mem, void *user) {
    if (!g_audio_hle) {
        cpu->r[0] = 0;
        return;
    }
    char name[192];
    uint32_t repeat = cpu->r[1];
    (void)user;
    snd_trace(user, cpu);
    gstr(mem, cpu->r[0], name, sizeof name);
    printf("  [mus  ] s3eAudioPlay(%s, repeat=%u)\n", name, (unsigned)repeat);
    cpu->r[0] = snd_music_play(name, repeat == 0u, g_audio_volume) ? 0u : 1u;
}

static void hle_audio_stop(GuestCpu *cpu, GuestMem *mem, void *user) {
    if (!g_audio_hle) {
        cpu->r[0] = 0;
        return;
    }
    (void)mem;
    snd_trace(user, cpu);
    snd_music_stop();
    cpu->r[0] = 0;
}

static void hle_audio_pause(GuestCpu *cpu, GuestMem *mem, void *user) {
    if (!g_audio_hle) {
        cpu->r[0] = 0;
        return;
    }
    (void)mem;
    snd_trace(user, cpu);
    snd_music_pause(1);
    cpu->r[0] = 0;
}

static void hle_audio_resume(GuestCpu *cpu, GuestMem *mem, void *user) {
    if (!g_audio_hle) {
        cpu->r[0] = 0;
        return;
    }
    (void)mem;
    snd_trace(user, cpu);
    snd_music_pause(0);
    cpu->r[0] = 0;
}

static void hle_audio_isplaying(GuestCpu *cpu, GuestMem *mem, void *user) {
    if (!g_audio_hle) {
        cpu->r[0] = 0;
        return;
    }
    (void)mem; (void)user;
    cpu->r[0] = (uint32_t)snd_music_playing();
}

/* A real free channel, not always 0 -- the game tracks them and would stack
 * every sound onto one slot. -1 is "none free", which we never need to say. */
static void hle_sound_getfreechannel(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t i;
    (void)mem; (void)user;
    snd_trace(user, cpu);
    /* Lowest free channel, not a rotation.
     *
     * The rotation was mine and it cost the game its sound: it configured
     * channel 0, played on it, and when the sound finished and it asked for a
     * free channel again it was handed 1 -- because the cursor had moved on --
     * whereupon it set no properties and played nothing. Marmalade returns the
     * lowest channel that is not playing, so a game that keeps per-channel
     * state of its own sees the same one back whenever it is idle. Spreading
     * voices around was solving a problem nobody had. */
    for (i = 0; i < SND_CHANNELS; i++) {
        uint32_t ch = i;
        if (!snd_channel_busy(ch)) {
            if (g_snd_free_shown < 40) {
                g_snd_free_shown++;
                printf("  [snd  ]   GetFreeChannel -> %u\n", (unsigned)ch);
            }
            cpu->r[0] = ch;
            return;
        }
    }
    /* Nothing free. Returning 0 here names a REAL channel, which tells the
     * game to go and use one that is already sounding; Marmalade reports -1
     * for this. Whether the game checks is exactly what the log will say. */
    if (g_snd_free_shown < 40) {
        g_snd_free_shown++;
        printf("  [snd  ]   GetFreeChannel -> none free, reporting -1\n");
    }
    cpu->r[0] = 0xFFFFFFFFu;
}

/* Start (or restart) the voice for what the channel currently holds: c->buf,
 * c->samples, and how it was first played (c->was_adpcm). Shared by Play and by
 * an end-of-sample handler asking for the sound to continue, so a repeat
 * decodes exactly the way the first pass did. */
/* Non-zero if a voice really started. The caller needs to know: STATUS is
 * answered from g_snd_playing now, so a play that starts nothing must not
 * leave the channel marked playing -- nothing would ever drain it, the game
 * would never take that channel back, and with sixteen of them a few such
 * wedges leave the quiet sounds (ambience) with nowhere to play. */
static int snd_start_voice(GuestMem *mem, uint32_t ch, SndChannel *c) {
    const uint32_t rate = c->rate ? c->rate : 22050u;
    if (!c->was_adpcm && snd_play_generated(mem, ch, c->buf, c->samples, rate,
                                            c->prop[3]))
        return 1;
    if (c->was_adpcm) {
        /* r2 counts compressed bytes in PAIRS, not samples: the descriptor
         * gives 11264 bytes for an r2 of 5632. */
        const uint32_t bytes = c->samples * 2u;
        const void *src = guest_ptr(mem, c->buf, bytes);
        uint32_t block = 512u;
        guest_ld32(mem, c->prop[2] + 0x28u, &block);
        if (block < 5u || block > 4096u)
            block = 512u;
        if (!src)
            return 0;
        {
            /* The descriptor 48 bytes ahead of the data states the sound's own
             * length and rate: +0x10 compressed bytes, +0x14 decoded samples,
             * +0x1c rate, +0x28 block. A sound that stops early is either
             * decoded short (fewer samples than +0x14 for the whole sound) or
             * played fast (our rate above the descriptor's), and this says
             * which -- guessing between them from the sound is hopeless. */
            uint32_t d_bytes = 0, d_samples = 0, d_rate = 0, got;
            static unsigned desc_log;
            guest_ld32(mem, c->prop[2] + 0x10u, &d_bytes);
            guest_ld32(mem, c->prop[2] + 0x14u, &d_samples);
            guest_ld32(mem, c->prop[2] + 0x1cu, &d_rate);
            got = snd_out_play(ch, src, bytes, block, rate, c->prop[3]);
            if (desc_log < 2000u &&
                (d_rate != rate || (d_bytes && bytes > d_bytes) ||
                 (uint64_t)got * 20ull < (uint64_t)d_samples * 19ull)) {
                desc_log++;
                printf("  [snd  ] ch%u MISMATCH rate=%u desc_rate=%u "
                       "bytes=%u/%u decoded=%u desc_samples=%u\n",
                       (unsigned)ch, (unsigned)rate, (unsigned)d_rate,
                       (unsigned)bytes, (unsigned)d_bytes, (unsigned)got,
                       (unsigned)d_samples);
            }
        }
    } else {
        /* No generator and no descriptor: plain 16-bit mono PCM, as the
         * s3eSound API itself defines the buffer. */
        const int16_t *src = (const int16_t *)guest_ptr(mem, c->buf,
                                                        c->samples * 2u);
        if (!src)
            return 0;
        snd_out_play_pcm(ch, src, c->samples, rate, c->prop[3]);
    }
    return 1;
}

/* s3eSoundChannelPlay(channel, start, numSamples, repeatCount, ...). */
static void hle_sound_channel_play(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t ch = cpu->r[0];
    static int peeked;
    snd_trace(user, cpu);
    if (ch < SND_CHANNELS) {
        SndChannel *c = &g_snd[ch];
        g_snd_playing[ch] = 1;
        g_snd_paused[ch] = 0;
        c->buf = cpu->r[1];
        c->samples = cpu->r[2];
        c->repeat = cpu->r[3];
        {
            /* Compact and nearly uncapped: which sound the game keeps
             * re-playing is a question about minutes of play, which the
             * call-count cap on snd_trace never reaches. */
            static unsigned play_log;
            if (play_log < 20000u) {
                /* Every play enters through one IwSound function, so lr alone
                 * names nothing. Return-address-looking words on the guest
                 * stack (odd = Thumb, inside the image) name who asked. */
                char bt[96];
                unsigned k, nbt = 0;
                size_t used = 0;
                bt[0] = 0;
                for (k = 0; k < 256u && nbt < 6u; k += 4u) {
                    uint32_t w = 0;
                    if (!guest_ld32(mem, cpu->r[13] + k, &w))   /* non-zero = ok */
                        break;
                    if ((w & 1u) && w >= 0x800000u && w < 0xc10000u) {
                        int n = snprintf(bt + used, sizeof bt - used, " %06x",
                                         (unsigned)(w - 0x800001u));
                        if (n < 0 || (size_t)n >= sizeof bt - used)
                            break;
                        used += (size_t)n;
                        nbt++;
                    }
                }
                play_log++;
                printf("  [snd  ] PLAY ch%u n=%u desc=%08x vol=%u lr=%08x bt(rva):%s\n",
                       (unsigned)ch, (unsigned)c->samples, (unsigned)c->prop[2],
                       (unsigned)c->prop[3], (unsigned)cpu->r[GUEST_LR], bt);
            }
        }
        /* The game's own generator first. Its sound manager registers an
         * S3E_CHANNEL_GEN_AUDIO callback on the channel for EVERY sample type
         * before it plays (RVA 0xd7626 / 0xd764e / 0xd7674) and expects the
         * platform to pull decoded audio from it -- which is exactly how the
         * PortMaster reference plays this game. Only one of the three types
         * happened to be IMA ADPCM with a descriptor we could decode ourselves,
         * so everything of the other two was silent. The native paths below
         * remain only for a play with no generator registered. */
        /* Except IMA ADPCM, which we decode natively. The game's ADPCM
         * generator (RVA 0xd76a9 -> 0xd65c8) reads the very descriptor it sets
         * through property 2 right before such a play, and running it cost
         * ~100 ms of guest time per sound on the game thread -- an audible
         * hitch every time one started. A descriptor set for THIS play is the
         * signal; the other sample types never set one. */
        c->was_adpcm = (uint8_t)(c->desc_fresh && c->prop[2]);
        c->desc_fresh = 0;
        snd_report_cut(ch, "replaced by a new play");
        if (!snd_start_voice(mem, ch, c))
            g_snd_playing[ch] = 0;   /* nothing to drain it otherwise */
        /* One look at the data, to settle 8- vs 16-bit and mono vs stereo.
         *
         * Read as int16, real audio is a smooth low-magnitude walk around
         * zero; if the data were 8-bit, pairing bytes would produce wild
         * neighbouring values instead. Twice is enough to see it, and it costs
         * nothing after that. */
        if (peeked < 2 && c->buf) {
            peeked++;
            printf("  [snd  ] ch%u play buf=%08x samples=%u rate=%u repeat=%u\n",
                   (unsigned)ch, (unsigned)c->buf, (unsigned)c->samples,
                   (unsigned)c->rate, (unsigned)c->repeat);
            /* The 48 bytes property 2 points at, which sit immediately before
             * the play buffer. A descriptor there would carry the channel
             * count and sample width, and a RIFF/WAV header would announce
             * itself in ASCII -- either settles the format outright, where
             * eight samples of waveform only invited guessing. */
            if (c->prop[2]) {
                char hex[3 * 48 + 1], asc[49];
                unsigned k, n = 0;
                for (k = 0; k < 48; k++) {
                    uint32_t b = 0;
                    if (!guest_ld8(mem, c->prop[2] + k, &b))
                        break;
                    n += (unsigned)snprintf(hex + n, sizeof hex - n, "%02x ",
                                            (unsigned)b);
                    asc[k] = (b >= 32u && b < 127u) ? (char)b : '.';
                }
                asc[k] = 0;
                if (k)
                    printf("  [snd  ]   header@%08x: %s |%s|\n",
                           (unsigned)c->prop[2], hex, asc);
            }
            /* And a shape summary of the payload rather than the payload:
             * how loud, how often it swings hard, and whether every other
             * byte is zero -- which is what 8-bit data widened to 16 looks
             * like. Enough to identify a format, and not a copy of anything. */
            {
                unsigned k, n = c->samples < 512u ? c->samples : 512u;
                unsigned loud = 0, lowzero = 0, got = 0;
                long sum = 0;
                int16_t mn = 32767, mx = -32768;
                for (k = 0; k < n; k++) {
                    uint32_t w = 0;
                    int16_t v;
                    if (!guest_ld16(mem, c->buf + k * 2u, &w))
                        break;
                    v = (int16_t)w;
                    got++;
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                    sum += v < 0 ? -(long)v : (long)v;
                    if (v > 16384 || v < -16384) loud++;
                    if ((w & 0xFFu) == 0u) lowzero++;
                }
                if (got)
                    printf("  [snd  ]   over %u int16: min=%d max=%d"
                           " mean|v|=%ld, %u%% beyond half-scale,"
                           " %u%% with a zero low byte\n",
                           got, mn, mx, sum / (long)got,
                           loud * 100u / got, lowzero * 100u / got);
            }
        }
    }
    cpu->r[0] = 0;                          /* S3E_RESULT_SUCCESS */
}

static void hle_sound_channel_stop(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t ch = cpu->r[0];
    static unsigned stop_log;
    (void)mem;
    snd_trace(user, cpu);
    /* Uncapped (well, 300): whether the game ever stops a looping sound is a
     * question about the END of a session, which the call-count cap on
     * snd_trace never reaches. */
    if (stop_log < 20000u) {
        stop_log++;
        printf("  [snd  ] stop ch%u (lr=%08x)\n", (unsigned)ch,
               (unsigned)cpu->r[GUEST_LR]);
    }
    if (ch < SND_CHANNELS) {
        snd_report_cut(ch, "stop");
        g_snd_playing[ch] = 0;
        g_snd_paused[ch] = 0;
        snd_out_stop(ch);
    }
    cpu->r[0] = 0;
}

/* s3eSoundChannelPause / Resume(channel). The game pauses every live sound
 * instance when its pause menu opens (RVA 0xd7356 walks the instance list and
 * calls each one's pause) and resumes them on the way out. Both used to be
 * no-ops, so whatever was playing carried on under the pause menu. STATUS
 * (GetInt 4) stays 1 while paused and PAUSED (GetInt 5) says 1: IsPlaying
 * (RVA 0xd85f4) reads the pair exactly that way. */
static void snd_channel_set_paused(GuestCpu *cpu, int paused) {
    uint32_t ch = cpu->r[0];
    if (ch < SND_CHANNELS && g_snd_playing[ch]) {
        g_snd_paused[ch] = paused ? 1u : 0u;
        snd_out_pause(ch, paused);
    }
    cpu->r[0] = 0;                          /* S3E_RESULT_SUCCESS */
}

static void hle_sound_channel_pause(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem;
    snd_trace(user, cpu);
    snd_channel_set_paused(cpu, 1);
}

static void hle_sound_channel_resume(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem;
    snd_trace(user, cpu);
    snd_channel_set_paused(cpu, 0);
}

/* s3eSoundChannelGetInt(channel, prop). Property 0 is STATUS: non-zero means
 * still playing, and a sound that never reports finished can wedge a queue. */
static void hle_sound_channel_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t ch = cpu->r[0], prop = cpu->r[1];
    (void)mem; (void)user;
    /* Property 0 is STATUS. It has to drain by itself: nothing sends an
     * end-of-sample event yet, so a channel that stays busy until an explicit
     * Stop is a channel the game will never reuse -- and with sixteen of them
     * that is silence a few seconds into a firefight. */
    /* Numbered as the reference: 0 RATE scale (0x10000 = as recorded), 1 RATE,
     * 2 user value, 3 VOLUME, 4 STATUS (playing), 5 PAUSED. Property 0 used to
     * be answered as status; the game has never polled it, so that was
     * harmless, but it is the kind of wrong answer that waits to bite. */
    if (ch >= SND_CHANNELS) {
        cpu->r[0] = 0xFFFFFFFFu;
    } else {
        switch (prop) {
        case 0:  cpu->r[0] = g_snd[ch].prop[0] ? g_snd[ch].prop[0] : 0x10000u; break;
        case 1:  cpu->r[0] = g_snd[ch].rate ? g_snd[ch].rate : 22050u; break;
        case 2:  cpu->r[0] = g_snd[ch].prop[2]; break;
        case 3:  cpu->r[0] = g_snd[ch].prop[3] ? g_snd[ch].prop[3] : 256u; break;
        case 4:  cpu->r[0] = (uint32_t)snd_channel_busy(ch); break;
        case 5:  cpu->r[0] = g_snd_paused[ch]; break;
        default: cpu->r[0] = 0xFFFFFFFFu; break;
        }
    }
    if (g_snd_stat_shown < 40) {
        g_snd_stat_shown++;
        printf("  [snd  ]   ChannelGetInt(ch%u, %u) -> %u\n",
               (unsigned)ch, (unsigned)prop, (unsigned)cpu->r[0]);
    }
}

static void hle_audio_ok(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem;
    snd_trace(user, cpu);
    cpu->r[0] = 0;                          /* S3E_RESULT_SUCCESS */
}

/* s3eSoundChannelSetInt(channel, property, value). Every distinct property is
 * reported once per channel rather than the first four calls overall, because
 * the interesting ones are set once at setup and then never again -- a
 * call-count cap hides exactly them. Property 1 carries 22050 in every trace
 * so far, which is a sample rate; the others are recorded without being
 * guessed at, which is how the ctype table and LowMemoryDevice were found. */
static void hle_sound_channel_setint(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t ch = cpu->r[0], prop = cpu->r[1], val = cpu->r[2];
    (void)mem; (void)user;
    if (ch < SND_CHANNELS) {
        SndChannel *c = &g_snd[ch];
        if (prop < 8) {
            c->prop[prop] = val;
            if (prop == 2u)
                c->desc_fresh = 1;
            if (!(c->prop_seen & (1u << prop))) {
                c->prop_seen |= 1u << prop;
                printf("  [snd  ] ch%u property %u = %u (0x%x)\n",
                       (unsigned)ch, (unsigned)prop, (unsigned)val,
                       (unsigned)val);
            }
        }
        /* 22050, 44100, 11025 and 8000 are the plausible rates; anything in
         * that band is the sample rate whichever property id carries it. */
        if (val >= 8000u && val <= 48000u)
            c->rate = val;
        /* Property 3 is the one the game sets to 128 before every sound, and
         * it is the only small non-rate value it sets, so it is the volume.
         * 256 is taken as unity; if everything turns out half as loud as it
         * should be, this scale is where to look. */
        if (prop == 3u) {
            static unsigned vol_log;
            static uint32_t last_vol[SND_CHANNELS];
            /* Changes only: distance fades and the fade-to-zero stop of a
             * looping sound are what this shows. */
            if (val != last_vol[ch] && vol_log < 5000u) {
                vol_log++;
                printf("  [snd  ] ch%u volume %u -> %u\n", (unsigned)ch,
                       (unsigned)last_vol[ch], (unsigned)val);
            }
            last_vol[ch] = val;
            snd_out_set_volume(ch, val);
        }
    }
    cpu->r[0] = 0;
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
 * changed the execution path at all, without diffing traces.
 *
 * All of them, not the top 20. The cut-off hid exactly the imports worth
 * finding: a subsystem that is set up once and then never used -- audio being
 * the case in hand -- makes a handful of calls and never appears, so the list
 * could not distinguish "never called" from "ranked 21st". Every import the
 * game touches is at most a few hundred lines and it is printed once per run. */
static unsigned g_stub_scratch[512];

/* Non-destructive, because it is now asked for DURING a run as well as at the
 * end of one. The ranking pass consumes what it prints, so it works on a copy;
 * zeroing the real counters mid-session would silently reset the very history
 * the next question depends on. */
static void dump_stub_calls(void) {
    unsigned i, k, named = 0;
    unsigned long long total = 0;
    for (i = 0; i < 512; i++) {
        g_stub_scratch[i] = g_stub_calls[i];
        if (g_stub_calls[i]) {
            named++;
            total += g_stub_calls[i];
        }
    }
    /* UNIMPLEMENTED, not "called": g_stub_calls is incremented in
     * hle_default, so an import with a handler -- every audio entry point, for
     * instance -- can be called constantly and never appear here. Reading this
     * list as the set of imports the game uses is wrong, and I did exactly
     * that once. */
    printf("\nunimplemented imports reaching the default stub"
           " (%u of them, %llu calls):\n", named, total);
    for (k = 0; k < 512; k++) {
        unsigned best = 0, bi = 0;
        for (i = 0; i < 512; i++)
            if (g_stub_scratch[i] > best) {
                best = g_stub_scratch[i];
                bi = i;
            }
        if (!best)
            break;
        printf("  %-34s %u\n", slot_name(bi), best);
        g_stub_scratch[bi] = 0;     /* consumed, so the next pass ranks below */
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

/* ---- network glue ------------------------------------------------------
 * net.c owns sockets and Play Online; it needs only the guest heap and a way
 * to call guest callbacks, which live here with `g`. */
#ifdef __SWITCH__
static uint32_t galloc(uint32_t n);
static uint32_t net_glue_alloc(uint32_t n) { return galloc(n); }
static int net_glue_call3(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2,
                          uint32_t *ret) {
    return guest_call_r0_3(&g, fn, a0, a1, a2, ret) == GUEST_OK;
}
static void net_pump_guest(void) {
    if (g_memp)
        net_pump(g_memp);
}
#else
static void net_pump_guest(void) {}
#endif

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
        /* Sound callbacks only. Whether these fire at all decides the
         * mixer's shape: an end-of-sample notification is something we send
         * when a buffer drains, while a generate callback is one the game
         * expects us to PULL from at audio rate -- and that second kind runs
         * guest code through the interpreter here, inside dynarmic, which is
         * the one path that does not get the JIT's speedup. */
        if (!strncmp(cb->kind, "s3eSound", 8) ||
            !strncmp(cb->kind, "s3eAudio", 8)) {
            static unsigned shown;
            if (shown < 8) {
                shown++;
                printf("  [snd  ] callback %s id=%u fn=%08x fired\n",
                       cb->kind, (unsigned)cb->id, (unsigned)cb->fn);
            }
        }
        st = guest_call(&g, cb->fn, g_cb_queue[i].sysdata, cb->user);
        if (st != GUEST_OK)
            printf("  [cb   ] %s id=%u stopped: %s\n", cb->kind,
                   (unsigned)cb->id, guest_status_str(st));
    }
}

/* ---- one-shot timers --------------------------------------------------
 *
 * s3eTimerSetTimer(ms, fn, userData) calls fn(NULL, userData) once, ms later;
 * s3eTimerCancelTimer(fn, userData) removes it. Both were unbound, so they fell
 * to hle_default -- "success" -- and the callback never ran.
 *
 * The main menu's music is exactly such a callback. GameStateFrontEnd arms a
 * 1500 ms timer (RVA 0x190ac2) whose handler (RVA 0x1908f4) asks the music
 * manager for blackops-music/mus_theatre_underscore.mp3, and cancels it again on
 * the way out (RVA 0x190b3a) just before stopping the music -- which is the
 * s3eAudioStop every session logged at the menu-to-game transition, stopping a
 * track that had never started.
 *
 * Fired from s3eDeviceYield like every other callback, because that is the one
 * point where re-entering guest code is safe. A timer is cleared before its
 * handler runs, so the handler is free to arm a new one. */
#define MAX_TIMERS 16
static struct { uint32_t fn, user, due; int used; } g_timers[MAX_TIMERS];
static unsigned g_timer_log;

static uint32_t timer_now_ms(void) {
    if (g_clock_fixed)
        return g_ticks;
    {
        uint64_t now = armGetSystemTick();
        if (!g_clock_base)
            g_clock_base = now;
        return (uint32_t)(((now - g_clock_base) * 1000ull) / armGetSystemTickFreq());
    }
}

/* s3eTimerGetUTC: uint64 milliseconds since 1970 in r0:r1. It was unbound, so
 * the game read 0 -- a clock stuck in 1970, which Demonware's auth, with its
 * ticket issue and expiry times, cannot work with. */
static void hle_timer_utc(GuestCpu *cpu, GuestMem *mem, void *user) {
    struct timespec ts;
    uint64_t ms = 0;
    (void)mem; (void)user;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    cpu->r[0] = (uint32_t)ms;
    cpu->r[1] = (uint32_t)(ms >> 32);
}

/* s3eTimerGetLocaltimeOffset: int64 ms. UTC is an honest answer. */
static void hle_timer_localoffset(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    cpu->r[0] = 0;
    cpu->r[1] = 0;
}

static void hle_timer_set(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t ms = cpu->r[0], fn = cpu->r[1], ud = cpu->r[2];
    int i, slot = -1;
    (void)mem; (void)user;
    /* Re-arming the same (fn, userData) restarts it rather than stacking a
     * second copy. */
    for (i = 0; i < MAX_TIMERS && slot < 0; i++)
        if (g_timers[i].used && g_timers[i].fn == fn && g_timers[i].user == ud)
            slot = i;
    for (i = 0; i < MAX_TIMERS && slot < 0; i++)
        if (!g_timers[i].used)
            slot = i;
    if (slot < 0 || !fn) {
        cpu->r[0] = 1;                      /* S3E_RESULT_ERROR */
        return;
    }
    g_timers[slot].fn = fn;
    g_timers[slot].user = ud;
    g_timers[slot].due = timer_now_ms() + ms;
    g_timers[slot].used = 1;
    if (g_timer_log < 40) {
        g_timer_log++;
        printf("  [timer] set %u ms fn=%08x user=%08x\n",
               (unsigned)ms, (unsigned)fn, (unsigned)ud);
    }
    cpu->r[0] = 0;                          /* S3E_RESULT_SUCCESS */
}

static void hle_timer_cancel(GuestCpu *cpu, GuestMem *mem, void *user) {
    uint32_t fn = cpu->r[0], ud = cpu->r[1];
    int i, n = 0;
    (void)mem; (void)user;
    for (i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].used && g_timers[i].fn == fn && g_timers[i].user == ud) {
            g_timers[i].used = 0;
            n++;
        }
    if (g_timer_log < 40) {
        g_timer_log++;
        printf("  [timer] cancel fn=%08x user=%08x (%d pending removed)\n",
               (unsigned)fn, (unsigned)ud, n);
    }
    cpu->r[0] = 0;
}

static void timer_pump(void) {
    uint32_t now = timer_now_ms();
    int i;
    for (i = 0; i < MAX_TIMERS; i++) {
        uint32_t fn, ud;
        GuestStatus st;
        if (!g_timers[i].used || (int32_t)(now - g_timers[i].due) < 0)
            continue;
        fn = g_timers[i].fn;
        ud = g_timers[i].user;
        g_timers[i].used = 0;
        if (g_timer_log < 40) {
            g_timer_log++;
            printf("  [timer] fire fn=%08x user=%08x\n", (unsigned)fn, (unsigned)ud);
        }
        st = guest_call(&g, fn, 0, ud);
        if (st != GUEST_OK)
            printf("  [timer] fn=%08x stopped: %s\n", (unsigned)fn,
                   guest_status_str(st));
    }
}

/* ---- sound generators -------------------------------------------------
 *
 * Rendered whole at Play time, on the guest thread, by calling the channel's
 * S3E_CHANNEL_GEN_AUDIO callback in chunks until it sets endSample -- the way
 * the PortMaster reference plays every sound in this game. Doing it at Play
 * time rather than from the mixer is what keeps guest code on the one thread
 * allowed to run it; sounds here are short, so the cost is a burst, not a
 * stream.
 *
 * The info block is s3eSoundGenAudioInfo: channel, target, numSamples, mix,
 * origStart, origNumSamples, origRepeat, endSample -- eight words, one scratch
 * block per channel, in guest memory because the callback writes through it. */
#define GEN_CHUNK 32768u    /* few calls per sound: each one is a guest round trip */
static uint32_t g_gen_target[SND_CHANNELS], g_gen_info[SND_CHANNELS];
static int16_t *g_gen_pcm;
static uint32_t g_gen_cap;
static unsigned g_gen_log;

static int snd_play_generated(GuestMem *mem, uint32_t ch, uint32_t start,
                              uint32_t samples, uint32_t rate, uint32_t volume) {
    int slot;
    uint32_t written = 0, limit, calls = 0;
    if (ch >= SND_CHANNELS)
        return 0;
    slot = cb_find_chan("s3eSoundChannel", ch, 1u);
    if (slot < 0 || !g_cbs[slot].used || !g_cbs[slot].fn)
        return 0;
    if (samples > 0x100000u)                /* bounds the expansion below */
        return 0;
    limit = samples * 8u + GEN_CHUNK;       /* ADPCM expands ~2x; generous */
    if (!g_gen_target[ch])
        g_gen_target[ch] = galloc(GEN_CHUNK * 2u);
    if (!g_gen_info[ch])
        g_gen_info[ch] = galloc(32u);
    if (!g_gen_target[ch] || !g_gen_info[ch])
        return 0;

    for (calls = 0; calls < 4096u; ) {
        uint32_t requested = GEN_CHUNK, produced = 0, end = 0;
        const void *src;
        GuestStatus st;
        if (written + requested > limit)
            requested = limit - written;
        if (!requested)
            break;
        guest_st32(mem, g_gen_info[ch] + 0u, ch);
        guest_st32(mem, g_gen_info[ch] + 4u, g_gen_target[ch]);
        guest_st32(mem, g_gen_info[ch] + 8u, requested);
        guest_st32(mem, g_gen_info[ch] + 12u, 0);         /* mix: no */
        guest_st32(mem, g_gen_info[ch] + 16u, start);
        guest_st32(mem, g_gen_info[ch] + 20u, samples);
        guest_st32(mem, g_gen_info[ch] + 24u, 0);         /* origRepeat */
        guest_st32(mem, g_gen_info[ch] + 28u, 0);         /* endSample */
        st = guest_call_r0(&g, g_cbs[slot].fn, g_gen_info[ch], g_cbs[slot].user,
                           &produced);
        calls++;
        if (st != GUEST_OK) {
            printf("  [snd  ] ch%u generator fn=%08x stopped: %s\n", (unsigned)ch,
                   (unsigned)g_cbs[slot].fn, guest_status_str(st));
            return 0;
        }
        guest_ld32(mem, g_gen_info[ch] + 28u, &end);
        if ((int32_t)produced < 0 || produced > requested)
            return 0;
        if (produced) {
            if (g_gen_cap < written + produced) {
                uint32_t cap = g_gen_cap ? g_gen_cap : GEN_CHUNK;
                int16_t *p;
                while (cap < written + produced)
                    cap *= 2u;
                p = (int16_t *)realloc(g_gen_pcm, cap * sizeof(int16_t));
                if (!p)
                    return 0;
                g_gen_pcm = p;
                g_gen_cap = cap;
            }
            src = guest_ptr(mem, g_gen_target[ch], produced * 2u);
            if (!src)
                return 0;
            memcpy(g_gen_pcm + written, src, produced * 2u);
            written += produced;
        }
        if (end || !produced)                /* finished, or no progress */
            break;
    }
    if (!written)
        return 0;
    snd_out_play_pcm(ch, g_gen_pcm, written, rate, volume);
    if (g_gen_log < 30) {
        g_gen_log++;
        printf("  [snd  ] ch%u generated %u samples from %u in %u call(s), fn=%08x\n",
               (unsigned)ch, (unsigned)written, (unsigned)samples,
               (unsigned)calls, (unsigned)g_cbs[slot].fn);
    }
    return 1;
}

/* ---- end of sample ----------------------------------------------------
 *
 * Marmalade calls the END_SAMPLE handler the moment a sample runs out and
 * takes its RETURN VALUE as the decision: non-zero keeps the channel playing --
 * the same data again, or whatever the handler left in newData/numSamples --
 * and zero lets it stop. (The PortMaster reference does exactly this in
 * service_finished_sound.) This port queued the handler for later and threw the
 * answer away, so every sound stopped after one pass.
 *
 * That is what made the teleporter loop forever. Its sound is meant to repeat a
 * fixed number of times: the game's handler (RVA 0xd8820) counts a repeat
 * counter down and keeps returning "continue" until it runs out. Stopped after
 * pass one instead, the game's own bookkeeping still believed the loop live
 * and kept re-triggering the sound from scratch -- endlessly, with gaps.
 *
 * The info block is s3eSoundEndSampleInfo: channel, repsRemaining, newData,
 * numSamples. */
static unsigned g_finish_log;

static void snd_finish_channel(uint32_t ch) {
    SndChannel *c;
    uint32_t info, reps, keep = 0, reps_after = 0, new_data = 0, new_n = 0;
    int slot, forever;
    unsigned k;
    if (ch >= SND_CHANNELS || !g_memp)
        return;
    c = &g_snd[ch];
    reps = c->repeat;
    forever = reps == 0u;
    if (reps > 0u)
        reps--;
    info = ENDINFO_ADDR(ch);
    for (k = 0; k < ENDINFO_STRIDE; k += 4u)
        guest_st32(g_memp, info + k, 0);
    guest_st32(g_memp, info + 0u, ch);
    guest_st32(g_memp, info + 4u, reps);
    guest_st32(g_memp, info + 12u, c->samples);

    slot = cb_find_chan("s3eSoundChannel", ch, 0u);
    if (slot >= 0 && g_cbs[slot].used && g_cbs[slot].fn) {
        GuestStatus st = guest_call_r0(&g, g_cbs[slot].fn, info, g_cbs[slot].user,
                                       &keep);
        if (st != GUEST_OK) {
            printf("  [snd  ] ch%u end handler stopped: %s\n", (unsigned)ch,
                   guest_status_str(st));
            keep = 0;
        }
    } else {
        keep = (forever || reps > 0u) ? 1u : 0u;
    }
    guest_ld32(g_memp, info + 4u, &reps_after);
    guest_ld32(g_memp, info + 8u, &new_data);
    guest_ld32(g_memp, info + 12u, &new_n);

    if (g_finish_log < 20000u) {
        /* The handler's own inputs, read from its instance (userData): spec at
         * +0xc with its loop count at +0x2c, repeats left at +0x28, and the
         * stop flag (bit 1) in +0x16. A CONTINUE with loop count 0 is an
         * infinite loop the game has to end by setting that flag. */
        uint32_t inst = slot >= 0 ? g_cbs[slot].user : 0, spec = 0, loops = 0,
                 left = 0, flags = 0;
        if (inst) {
            guest_ld32(g_memp, inst + 0x0cu, &spec);
            guest_ld32(g_memp, inst + 0x28u, &left);
            guest_ld16(g_memp, inst + 0x16u, &flags);
            if (spec)
                guest_ld32(g_memp, spec + 0x2cu, &loops);
        }
        g_finish_log++;
        printf("  [snd  ] ch%u finished: handler says %s (reps %u, newData %08x, n %u)"
               " inst=%08x spec=%08x loops=%u left=%u flags=%04x\n",
               (unsigned)ch, keep ? "CONTINUE" : "stop", (unsigned)reps_after,
               (unsigned)new_data, (unsigned)new_n, (unsigned)inst,
               (unsigned)spec, (unsigned)loops, (unsigned)left, (unsigned)flags);
    }
    if (!keep) {
        g_snd_playing[ch] = 0;
        return;
    }
    if (new_data && new_n) {
        c->buf = new_data;
        c->samples = new_n;
    }
    c->repeat = (int32_t)reps_after > 0 ? reps_after : 0u;
    /* Same rule as a fresh play: only claim the channel if a voice started. */
    g_snd_playing[ch] = (uint8_t)(snd_start_voice(g_memp, ch, c) ? 1 : 0);
}

static void bind_slot(uint32_t i, const char *nm) {
    g_slots[i].name = nm;
    g_slots[i].user = (void *)(uintptr_t)i;
    g_slots[i].fn = hle_default;
    if (!nm)
        return;
#ifdef __SWITCH__
    {   /* s3eSocket / s3eInet: net.c */
        GuestHleFn netfn = net_find_hle(nm);
        if (netfn) {
            g_slots[i].fn = netfn;
            return;
        }
    }
#endif
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
    else if (!strcmp(nm, "s3eSoundChannelSetInt"))
        g_slots[i].fn = hle_sound_channel_setint;
    else if (!strcmp(nm, "s3eSoundChannelPause"))
        g_slots[i].fn = hle_sound_channel_pause;
    else if (!strcmp(nm, "s3eSoundChannelResume"))
        g_slots[i].fn = hle_sound_channel_resume;
    else if (!strcmp(nm, "s3eAudioGetInt"))
        g_slots[i].fn = hle_audio_getint;
    else if (!strcmp(nm, "s3eAudioSetInt"))
        g_slots[i].fn = hle_audio_setint;
    else if (!strcmp(nm, "s3eAudioPlay"))
        g_slots[i].fn = hle_audio_play;
    else if (!strcmp(nm, "s3eAudioStop"))
        g_slots[i].fn = hle_audio_stop;
    else if (!strcmp(nm, "s3eAudioPause"))
        g_slots[i].fn = hle_audio_pause;
    else if (!strcmp(nm, "s3eAudioResume"))
        g_slots[i].fn = hle_audio_resume;
    /* PlayFromBuffer hands us the encoded data in guest memory instead of a
     * filename. Nothing has been seen to call it, so it keeps the old
     * stand-in rather than a decoder path that cannot be tested. */
    else if (!strcmp(nm, "s3eAudioPlayFromBuffer"))
        g_slots[i].fn = hle_audio_ok;
    else if (!strcmp(nm, "s3eAudioIsPlaying"))
        g_slots[i].fn = hle_audio_isplaying;
    else if (!strcmp(nm, "s3eMemoryGetInt"))
        g_slots[i].fn = hle_memory_getint;
    else if (!strcmp(nm, "s3eMemorySetInt"))
        g_slots[i].fn = hle_memory_setint;
    else if (!strcmp(nm, "s3eTimerGetMs") || !strcmp(nm, "s3eTimerGetUST"))
        g_slots[i].fn = hle_timer_ms;
    else if (!strcmp(nm, "s3eTimerGetUTC"))
        g_slots[i].fn = hle_timer_utc;
    else if (!strcmp(nm, "s3eTimerGetLocaltimeOffset"))
        g_slots[i].fn = hle_timer_localoffset;
    else if (!strcmp(nm, "s3eTimerSetTimer"))
        g_slots[i].fn = hle_timer_set;
    else if (!strcmp(nm, "s3eTimerCancelTimer"))
        g_slots[i].fn = hle_timer_cancel;
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
    else if (!strcmp(nm, "s3eKeyboardGetInt"))
        g_slots[i].fn = hle_key_getint;
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

/* The breadcrumb has done its job once the game is actually running, and
 * leaving it on the card makes the next launch stop and ask to be
 * acknowledged -- every launch, forever, because nothing ever removed it.
 * It should only speak up when the previous run died BEFORE it got going. */
static void startup_stage_clear(void) {
    unsigned i;
    for (i = 0; i < sizeof(g_stage_paths) / sizeof(g_stage_paths[0]); i++)
        remove(g_stage_paths[i]);
}


/* ---- advanced settings ---------------------------------------------------
 *
 * The development switches: the code cache size, the profilers, benchmark
 * mode, the fixed clock, the touch markers and the log host. Keys in
 * config.txt, edited from the menu's Advanced tab.
 *
 * They used to be files on the card (dynarmic.txt, profile.txt, bench.txt
 * and so on). Those are still read ONCE each, and only where the matching key
 * is absent, so a card set up the old way is written into config.txt on the
 * next launch. Nothing deletes them; they stop being consulted as soon as the
 * key exists. Flag files for engines that no longer exist (jit.txt,
 * fastmem.txt, recomp.txt, ...) are simply ignored.
 *
 * All of these take effect at launch and are read once, here: what the menu
 * edits is the next run, not this one.
 */

/* Set when the previous launch died before it finished starting. Every
 * advanced key is then ignored for one run, because the usual reason a launch
 * dies early is the setting someone just changed -- and the menu that would
 * change it back is on the far side of the boot that is failing. */
static int g_safe_mode;

/* What the run actually ended up using, for the menu to show. Built as each
 * switch is read, because "what was asked for" and "what is running" differ
 * under safe mode, and again when dynarmic declines to start. */
static char g_runtime_label[192];

static void runtime_note(const char *text) {
    size_t n = strlen(g_runtime_label);
    if (n + 3 >= sizeof g_runtime_label)
        return;
    if (n) {
        g_runtime_label[n++] = ',';
        g_runtime_label[n++] = ' ';
    }
    snprintf(g_runtime_label + n, sizeof g_runtime_label - n, "%s", text);
}

/* A legacy flag file, in either directory they were accepted from. Returns 1
 * if it exists; *num is the number it carried, or -1 for none. */
static int legacy_flag(const char *name, long *num) {
    char path[96];
    unsigned i;
    *num = -1;
    for (i = 0; i < 2; i++) {
        FILE *f;
        snprintf(path, sizeof path, i ? "sdmc:/%s" : "sdmc:/switch/boz/%s", name);
        f = fopen(path, "rb");
        if (!f)
            continue;
        if (fscanf(f, "%ld", num) != 1)
            *num = -1;
        fclose(f);
        return 1;
    }
    return 0;
}

/* The same, for a file whose CONTENTS were the value rather than a number:
 * nxlink_host.txt held an address. */
static int legacy_text(const char *name, char *out, size_t cap) {
    char path[96];
    unsigned i;
    for (i = 0; i < 2; i++) {
        FILE *f;
        size_t n;
        snprintf(path, sizeof path, i ? "sdmc:/%s" : "sdmc:/switch/boz/%s", name);
        f = fopen(path, "rb");
        if (!f)
            continue;
        n = fread(out, 1, cap - 1, f);
        fclose(f);
        out[n] = 0;
        while (n && (unsigned char)out[n - 1] <= ' ')
            out[--n] = 0;
        return 1;
    }
    return 0;
}

static int settings_have(const char *key) {
    return settings_get(key, NULL) != NULL;
}

/* What the import did, reported later: this all happens before stdout is
 * redirected to the development host, so anything printed here would land on
 * the console screen and never reach the log. */
static unsigned g_imported;
static int      g_import_saved;

/* The one-time import. A key already in config.txt always wins: this exists
 * for cards that predate it, not to keep the files authoritative. */
static void advanced_import_legacy(void) {
    static const struct { const char *key; const char *file; } presence[] = {
        { "profilers",     "profile.txt" },
        { "fixed_clock",   "fixedclock.txt" },
        { "touch_debug",   "touchdbg.txt" },
    };
    unsigned i, n = 0;
    long v;

    for (i = 0; i < sizeof presence / sizeof presence[0]; i++) {
        if (settings_have(presence[i].key) || !legacy_flag(presence[i].file, &v))
            continue;
        settings_set_int(presence[i].key, 1);
        n++;
    }
    /* dynarmic.txt chose the engine and carried the cache size; only the
     * size is still a choice. */
    if (legacy_flag("dynarmic.txt", &v) && v > 0 &&
        !settings_have("dynarmic_cache_mb")) {
        settings_set_int("dynarmic_cache_mb", (int)v);
        n++;
    }
    if (legacy_flag("bench.txt", &v)) {
        if (!settings_have("bench")) {
            settings_set_int("bench", 1);
            n++;
        }
        if (v > 0 && !settings_have("bench_million")) {
            settings_set_int("bench_million", (int)v);
            n++;
        }
    }
    /* Not a flag: the development host stdout is streamed to. Its file held
     * an address rather than a number, so it is read as text. */
    if (!settings_have("nxlink_host")) {
        char host[64];
        if (legacy_text("nxlink_host.txt", host, sizeof host) && host[0]) {
            settings_set("nxlink_host", host);
            n++;
        }
    }
    if (n) {
        g_imported = n;
        g_import_saved = settings_save() != 0;
    }
}

/* From run(), once the log is going where it can be read. */
static void advanced_report(void) {
    if (g_imported)
        printf("  [cfg  ] imported %u old flag file setting(s) into config.txt%s\n",
               g_imported, g_import_saved ? "" : " (SAVE FAILED)");
    if (g_imported)
        printf("  [cfg  ] the .txt flag files are no longer read; delete them\n");
    if (g_safe_mode)
        printf("  [cfg  ] SAFE MODE: the last launch did not finish starting,"
               " so every Advanced setting is ignored this run\n");
}

/* Read an advanced key. Under safe mode every one returns its built-in
 * default, whatever the file says. */
static int adv_int(const char *key, int def) {
    return g_safe_mode ? def : settings_get_int(key, def);
}

/* From main(), once the card is readable and the crash breadcrumb has been
 * read: that breadcrumb is what decides safe mode. */
static void advanced_init(int previous_run_died) {
    settings_load();
    advanced_import_legacy();
    g_safe_mode = previous_run_died;
}

/* For the menu (menu.h). */
int port_safe_mode(void) {
    return g_safe_mode;
}

const char *port_runtime_label(void) {
    return g_runtime_label[0] ? g_runtime_label : "starting";
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

static unsigned g_dyn_mb = 32;      /* code cache, MB; dynarmic_cache_mb */
static int      g_dyn_live;         /* dyn_init actually succeeded */

/* Page-aligned, zeroed: the guest regions are handed to dynarmic's page
 * table in 4 KB pages, and a region that started mid-page would lose its
 * first partial page to the slow path. */
static uint8_t *page_alloc(size_t n) {
    size_t sz = (n + 0xFFFu) & ~(size_t)0xFFFu;
    void *p = memalign(0x1000, sz);
    if (!p)
        return NULL;
    memset(p, 0, sz);
    return (uint8_t *)p;
}

/* ------------------------------------------------------- control socket ---
 *
 * The NRO listens for development. It already speaks to the PC in one
 * direction (stdout over nxlink); this is the other, a small line protocol on
 * port 28772 that writes, deletes and lists files under sdmc:/switch/boz --
 * which is how a new build (the NRO lives there) and config.txt get onto the
 * card without ejecting it -- plus the SND commands that drive a running game.
 *
 * Files written take effect at the next launch, because settings are read
 * once during startup.
 *
 * Confined to sdmc:/switch/boz by construction -- no separators, no "..", no
 * control characters, bounded length. This is an unauthenticated listener on
 * the local network, so the confinement is the whole security model: the worst
 * a stranger on the LAN can do is rewrite this homebrew's own files.
 *
 * Non-blocking throughout, polled once per 5M-instruction chunk (about four
 * times a second), so it cannot stall the guest or add jitter to a benchmark.
 */
#define CTL_PORT 28772
#define CTL_DIR  "sdmc:/switch/boz"

static int   ctl_listen = -1, ctl_client = -1;
static char  ctl_line[256];
static int   ctl_line_n;
static int   ctl_put_left;
static FILE *ctl_put_f;

static void ctl_say(const char *s) {
    if (ctl_client >= 0)
        send(ctl_client, s, strlen(s), 0);
}

static void ctl_drop(void) {
    if (ctl_put_f) { fclose(ctl_put_f); ctl_put_f = NULL; }
    if (ctl_client >= 0) { close(ctl_client); ctl_client = -1; }
    ctl_line_n = 0;
    ctl_put_left = 0;
}

/* A bare filename and nothing else. Rejecting separators outright is cruder
 * than resolving the path, and unlike resolving it there is nothing to get
 * subtly wrong. */
/* Confined to CTL_DIR, now including subdirectories beneath it.
 *
 * The guard used to reject '/' outright, which made the channel useless for
 * the game's own data: the assets live in directories (blackops-music/,
 * data-etc/) and could only be put there by hand. Allowing a separator does
 * not weaken the confinement as long as the path cannot climb out of it or
 * start at the root, so those are what is checked -- no leading '/', no ".."
 * anywhere, no empty component, no backslash, nothing below space.
 *
 * This is still an unauthenticated listener on the local network, and the
 * confinement is still the whole security model. The worst a stranger on the
 * LAN can now do is write a file into a subdirectory of this homebrew's own
 * folder rather than only into the folder itself. */
static int ctl_safe(const char *n) {
    const char *p;
    if (!*n || *n == '/' || strlen(n) > 96)
        return 0;
    for (p = n; *p; p++) {
        if (*p == '\\' || (unsigned char)*p < 32)
            return 0;
        if (*p == '/' && (p[1] == '/' || p[1] == 0))
            return 0;                   /* empty component */
    }
    return strstr(n, "..") == NULL;
}

/* Create the directories leading to a path under CTL_DIR. Called only from
 * PUT, and only on a name ctl_safe has already accepted. */
static void ctl_mkparents(const char *rel) {
    char path[224];
    char *slash;
    snprintf(path, sizeof path, CTL_DIR "/%s", rel);
    for (slash = strchr(path + sizeof(CTL_DIR), '/'); slash;
         slash = strchr(slash + 1, '/')) {
        *slash = 0;
        mkdir(path, 0777);              /* already-exists is the normal case */
        *slash = '/';
    }
}

/* Texture bisection, in gl_egl.c. */
void gl_tex_list(void);
int  gl_tex_skip(unsigned id);
int  gl_tex_dump(unsigned id);
void gl_hide_sticks(int on);
int  gl_sticks_hidden(void);
void gl_show_fps(int on);
int  gl_fps_shown(void);
void gl_stick_probe(int n);
unsigned gl_hidden_draws(void);

#ifdef __SWITCH__
/* ---- settings bridge ----------------------------------------------------
 * The in-game menu and config.txt name settings by key; these are the live
 * values behind them, the same ones the control socket's SND commands change.
 * Keys that apply only at boot (multiplayer_server, player_name) are not here:
 * net.c reads those from the file itself. */

/* Aim speed, as a percentage of the defaults, applied to BOTH ways the right
 * stick can reach the game. With a controller the game normally takes the
 * Xperia touchpad, and there turn rate is set by the look pad's radius --
 * so a setting that only moved g_aim_speed (the touch-emulation fallback)
 * did nothing at all in normal play.
 *
 * The radius is capped where the pad would clip: the look pad is centred at
 * 4/5 of the width, so beyond 191 a full right deflection hits the edge while
 * a full left one does not, and turning right would top out slower than
 * turning left. That caps the pad path at ~160%.
 *
 * Vertical has its own percentage and more room: the pad is 544 tall and
 * centred, so the radius can reach 271 (~225%) before the edge clips. */
#define AIM_PCT_MIN   50
#define AIM_PCT_MAX   160
#define AIM_Y_PCT_MAX 225
static int g_aim_pct = 100;
static int g_aim_y_pct = 100;

static void aim_y_apply(int pct) {
    const int look_max = TP_H / 2 - 1;
    int r;
    g_aim_y_pct = pct < AIM_PCT_MIN ? AIM_PCT_MIN
                : (pct > AIM_Y_PCT_MAX ? AIM_Y_PCT_MAX : pct);
    r = (TP_W / 8) * g_aim_y_pct / 100;
    g_tp_look_ry = r > look_max ? look_max : r;
    g_aim_speed_y = PX_W(53) * g_aim_y_pct / 100;
}

static void aim_apply(int pct) {
    const int look_max = TP_W - 1 - TP_W * 4 / 5;
    int r;
    g_aim_pct = pct < AIM_PCT_MIN ? AIM_PCT_MIN
              : (pct > AIM_PCT_MAX ? AIM_PCT_MAX : pct);
    r = (TP_W / 8) * g_aim_pct / 100;
    g_tp_look_r = r > look_max ? look_max : r;
    g_aim_speed = PX_W(53) * g_aim_pct / 100;
}

int port_setting_get(const char *key) {
    if (!strcmp(key, "control_layout")) return g_tp_on;
    if (!strcmp(key, "aim_hold"))       return g_aim_hold;
    if (!strcmp(key, "run_toggle"))     return g_run_toggle;
    if (!strcmp(key, "y_hold_frames"))  return g_y_use_delay;
    if (!strcmp(key, "aim_stick"))      return g_aim_stick;
    if (!strcmp(key, "aim_sensitivity")) return g_aim_pct;
    if (!strcmp(key, "aim_sensitivity_y")) return g_aim_y_pct;
    if (!strcmp(key, "hide_sticks"))    return gl_sticks_hidden();
    if (!strcmp(key, "show_fps"))       return gl_fps_shown();
    if (!strcmp(key, "music"))          return snd_out_music_enabled();
    return 0;
}

void port_setting_set(const char *key, int v) {
    if (!strcmp(key, "control_layout"))     g_tp_on = v ? 1 : 0;
    else if (!strcmp(key, "aim_hold"))      g_aim_hold = v ? 1 : 0;
    else if (!strcmp(key, "run_toggle"))    g_run_toggle = v ? 1 : 0;
    else if (!strcmp(key, "y_hold_frames")) g_y_use_delay = v < 1 ? 1 : (v > 120 ? 120 : v);
    else if (!strcmp(key, "aim_stick"))     g_aim_stick = v ? 1 : 0;
    else if (!strcmp(key, "aim_sensitivity")) aim_apply(v);
    else if (!strcmp(key, "aim_sensitivity_y")) aim_y_apply(v);
    else if (!strcmp(key, "hide_sticks"))   gl_hide_sticks(v ? 1 : 0);
    else if (!strcmp(key, "show_fps"))      gl_show_fps(v ? 1 : 0);
    else if (!strcmp(key, "music"))         snd_out_music_enable(v ? 1 : 0);
}

const char *port_build_label(void) {
    return BOZ_BUILD_LABEL;
}

/* Everything config.txt sets, applied once at boot; absent keys keep the
 * built-in defaults. */
static void port_settings_apply(void) {
    static const char *keys[] = {
        "control_layout", "aim_hold", "run_toggle", "y_hold_frames", "aim_stick",
        "aim_sensitivity", "aim_sensitivity_y", "hide_sticks", "show_fps",
        "music",
    };
    unsigned i, applied = 0;
    /* advanced_init read the file at startup; reloading here would throw
     * away nothing, but it would also invite a second source of truth. */
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const int current = port_setting_get(keys[i]);
        const int value = settings_get_int(keys[i], current);
        if (value != current) {
            port_setting_set(keys[i], value);
            applied++;
        }
    }
    printf("  [menu ] config.txt: %u setting(s) applied; hold - for 2 s for the"
           " menu\n", applied);
}
#endif

static void ctl_command(char *line) {
    char *arg = strchr(line, ' ');
    if (arg)
        *arg++ = 0;

    if (!strcmp(line, "LS")) {
        DIR *d = opendir(CTL_DIR);
        struct dirent *e;
        if (!d) { ctl_say("ERR no dir\n"); return; }
        while ((e = readdir(d))) {
            ctl_say(e->d_name);
            ctl_say("\n");
        }
        closedir(d);
        ctl_say(".\n");
    } else if (!strcmp(line, "SND") && arg) {
        /* SND ENDCB 0|1 -- fire the end-of-sample callback, or do not.
         *
         * Takes effect immediately, so both behaviours can be compared inside
         * one session by ear, which is the only instrument that can judge it.
         *
         * Note the shape: ctl_command has ALREADY split the line at the first
         * space, so the verb is `line` and everything after it is `arg`. The
         * first version of this matched on "SND " with the space still in it
         * and could never fire -- every other command here is written the
         * right way, immediately above. */
        if (!strcmp(arg, "AUDIOHLE 0") || !strcmp(arg, "AUDIOHLE 1")) {
            g_audio_hle = arg[9] == '1';
            printf("  [snd  ] s3eAudio handlers %s\n",
                   g_audio_hle ? "LIVE" : "stubbed (pre-music behaviour)");
            ctl_say("OK\n");
        } else if (!strncmp(arg, "TAP ", 4)) {
            int x = 0, y = 0;
            if (sscanf(arg + 4, "%d %d", &x, &y) == 2 &&
                x >= 0 && x < (int)SCREEN_W && y >= 0 && y < (int)SCREEN_H) {
                g_act_x = x;
                g_act_y = y;
                printf("  [pad  ] action tap at (%d,%d)\n", x, y);
                ctl_say("OK\n");
            } else {
                ctl_say("ERR use: SND TAP <x> <y>\n");
            }
        } else if (!strncmp(arg, "KEY ", 4)) {
            /* Pulse one key code: down now, up on the next poll. The way to
             * name a code is to send it and see what the game does, which
             * beats a fourth round of guessing from someone else's table. */
            int code = atoi(arg + 4);
            if (code > 0 && code < 512) {
                g_key_pulse = (uint32_t)code;
                printf("  [key  ] pulsing code %d\n", code);
                ctl_say("OK\n");
            } else {
                ctl_say("ERR use: SND KEY <1..511>\n");
            }
        } else if (!strncmp(arg, "AIM ", 4)) {
            /* SND AIM <speed> [radius] -- px per frame, and the held-stick
             * deflection. Both are feel, and feel cannot be measured from
             * here. */
            int sp = 0, rad = 0;
            if (sscanf(arg + 4, "%d %d", &sp, &rad) >= 1 && sp > 0 && sp < 64) {
                g_aim_speed = sp;
                if (rad > 8 && rad < 200)
                    g_aim_radius = rad;
                printf("  [pad  ] aim speed %d, radius %d\n",
                       g_aim_speed, g_aim_radius);
                ctl_say("OK\n");
            } else {
                ctl_say("ERR use: SND AIM <speed> [radius]\n");
            }
        } else if (!strcmp(arg, "TEXLIST")) {
            gl_tex_list();
            ctl_say("OK\n");
        } else if (!strncmp(arg, "SKIPTEX ", 8)) {
            int r = gl_tex_skip((unsigned)atoi(arg + 8));
            ctl_say(r < 0 ? "ERR use: SND SKIPTEX <id> (0 clears)\n"
                          : "OK\n");
        } else if (!strncmp(arg, "DUMPTEX ", 8)) {
            int r = gl_tex_dump((unsigned)atoi(arg + 8));
            ctl_say(r == 0 ? "OK\n" : "ERR dump failed\n");
        } else if (!strncmp(arg, "STICKPROBE ", 11)) {
            int n = atoi(arg + 11);
            if (n > 0 && n <= 20000) {
                gl_stick_probe(n);
                ctl_say("OK\n");
            } else {
                ctl_say("ERR use: SND STICKPROBE <1..20000>\n");
            }
        } else if (!strcmp(arg, "HIDESTICKS 0") || !strcmp(arg, "HIDESTICKS 1")) {
            gl_hide_sticks(arg[11] == '1');
            printf("  [tex  ] virtual sticks %s (%u draws hidden so far)\n",
                   arg[11] == '1' ? "hidden" : "shown", gl_hidden_draws());
            ctl_say("OK\n");
        } else if (!strncmp(arg, "YHOLD ", 6)) {
            int f = atoi(arg + 6);
            if (f >= 1 && f <= 120) {
                g_y_use_delay = f;
                printf("  [key  ] Y on the move: use after %d frames held\n", f);
                ctl_say("OK\n");
            } else {
                ctl_say("ERR use: SND YHOLD <1..120>\n");
            }
        } else if (!strcmp(arg, "MENU 0") || !strcmp(arg, "MENU 1")) {
            menu_set_open(arg[5] == '1');   /* the settings menu, without the hold */
            ctl_say("OK\n");
        } else if (!strcmp(arg, "AIMHOLD 0") || !strcmp(arg, "AIMHOLD 1")) {
            g_aim_hold = arg[8] == '1';
            printf("  [key  ] aim: %s\n",
                   g_aim_hold ? "hold ZL to aim" : "press ZL to toggle aim");
            ctl_say("OK\n");
        } else if (!strcmp(arg, "RUNTOGGLE 0") || !strcmp(arg, "RUNTOGGLE 1")) {
            g_run_toggle = arg[10] == '1';
            printf("  [key  ] run: %s\n",
                   g_run_toggle ? "click L3 to run" : "hold L3 to run");
            ctl_say("OK\n");
        } else if (!strncmp(arg, "TAPMAX ", 7)) {
            int f = atoi(arg + 7);
            if (f >= 0 && f <= 120) {
                g_use_cap = f;
                printf("  [key  ] use cap %d frames\n", f);
                ctl_say("OK\n");
            } else {
                ctl_say("ERR use: SND TAPMAX <0..120>\n");
            }
        } else if (!strncmp(arg, "HOLD ", 5)) {
            int f = atoi(arg + 5);
            if (f >= 0 && f <= 120) {
                g_reload_hold = f;
                printf("  [key  ] reload min hold %d frames\n", f);
                ctl_say("OK\n");
            } else {
                ctl_say("ERR use: SND HOLD <0..120>\n");
            }
        } else if (!strncmp(arg, "TPLOOK ", 7)) {
            int r = atoi(arg + 7);
            if (r > 0 && r <= TP_W / 2) {
                g_tp_look_r = r;
                printf("  [tpad ] look radius %d\n", g_tp_look_r);
                ctl_say("OK\n");
            } else {
                ctl_say("ERR use: SND TPLOOK <1..480>\n");
            }
        } else if (!strcmp(arg, "TPAD 0") || !strcmp(arg, "TPAD 1")) {
            g_tp_on = arg[5] == '1';
            printf("  [tpad ] %s (%d listener(s))\n", g_tp_on
                   ? "analog pad" : "off, synthetic touch instead",
                   tp_listeners());
            ctl_say("OK\n");
        } else if (!strcmp(arg, "AIMMODE 0") || !strcmp(arg, "AIMMODE 1")) {
            g_aim_stick = arg[8] == '1';
            printf("  [pad  ] right stick: %s\n",
                   g_aim_stick ? "held stick (dual-stick mode)"
                               : "drag and re-anchor (swipe-aim mode)");
            ctl_say("OK\n");
        } else if (!strcmp(arg, "KEYS")) {
            /* Codes ranked by how often the game asked, with how often we
             * sent them alongside. A high poll count and a zero send count is
             * a code this build cares about that nothing is mapped to yet. */
            unsigned k, shown = 0;
            printf("  [key  ] code  polled   sent\n");
            for (; shown < 16u; shown++) {
                unsigned best = 0, bi = 0;
                for (k = 0; k < 512u; k++)
                    if (g_key_polls[k] > best) { best = g_key_polls[k]; bi = k; }
                if (!best)
                    break;
                printf("  [key  ] %4u  %7u  %5u\n",
                       bi, best, (unsigned)g_key_sends[bi]);
                g_key_polls[bi] = 0;   /* consumed for the ranking pass */
            }
            ctl_say("OK\n");
        } else if (!strcmp(arg, "STAT")) {
            /* Is the mixer thread alive? Ask twice and compare the count. */
            printf("  [snd  ] mix=%llu voices=%u music=%s\n",
                   (unsigned long long)snd_out_mix_calls(),
                   snd_out_active_voices(),
                   snd_music_playing() ? "playing" : "idle");
            ctl_say("OK\n");
        } else if (!strcmp(arg, "SELFTEST")) {
            /* Plays a sound longer than the mixer's old position limit and
             * reports how long it took to drain. Needs no barricade. */
            snd_selftest_start();
            ctl_say("OK\n");
        } else if (!strcmp(arg, "MUSIC 0") || !strcmp(arg, "MUSIC 1")) {
            snd_out_music_enable(arg[6] == '1');
            printf("  [snd  ] music mixing %s\n", arg[6] == '1' ? "ON" : "OFF");
            ctl_say("OK\n");
        } else if (!strcmp(arg, "ENDCB 0") || !strcmp(arg, "ENDCB 1")) {
            g_snd_endcb = arg[6] == '1';
            printf("  [snd  ] end-of-sample callback %s\n",
                   g_snd_endcb ? "ON" : "OFF");
            ctl_say(g_snd_endcb ? "OK endcb on\n" : "OK endcb off\n");
        } else {
            ctl_say("ERR use: SND STAT|KEYS|AIM n|AIMMODE 0|1|MUSIC 0|1\n");
        }
    } else if (!strcmp(line, "STUBS")) {
        /* The import call counts, on demand. They used to be available only on
         * the way out, which is useless for a question like "does this game
         * ever call s3eAudioPlay" -- answering it meant quitting, and quitting
         * meant losing the state that would have produced the call. */
        dump_stub_calls();
        ctl_say("OK\n");
    } else if (!strcmp(line, "DIAG")) {
        /* The one command that is useful precisely when nothing else is.
         *
         * This runs on the control socket's thread, which keeps being
         * scheduled while the guest thread is wedged inside the JIT -- so it
         * can report what the guest is doing when the guest has stopped saying
         * anything. The output goes to the log rather than back down the
         * socket because that is where the rest of the run is, and reading it
         * next to the last few lines before the silence is the whole point.
         *
         * Ask twice and compare: counters that move mean a loop, counters that
         * do not mean it is stuck in one place. */
        dyn_diag();
        ctl_say("OK\n");
    } else if (!strcmp(line, "GET") && arg && ctl_safe(arg)) {
        /* Reads a file back off the card -- PUT, DEL and LS can change it but
         * never report its contents. Replies "OK <len>" and then that many
         * raw bytes.
         *
         * The send loop spins on EAGAIN because the client socket is
         * non-blocking and a file larger than the socket buffer would
         * otherwise be silently truncated. Spinning is acceptable here and
         * nowhere else: this runs only when a human asked for a file, never
         * on the guest's path. */
        char path[160];
        FILE *fp = NULL;
        long len = 0;
        snprintf(path, sizeof path, CTL_DIR "/%s", arg);
        fp = fopen(path, "rb");
        if (!fp) {
            ctl_say("ERR open\n");
            return;
        }
        fseek(fp, 0, SEEK_END);
        len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        {
            char hdr[48];
            snprintf(hdr, sizeof hdr, "OK %ld\n", len);
            ctl_say(hdr);
        }
        {
            char b[4096];
            size_t n;
            while ((n = fread(b, 1, sizeof b, fp)) > 0) {
                size_t off = 0;
                while (off < n) {
                    int w = send(ctl_client, b + off, n - off, 0);
                    if (w > 0)
                        off += (size_t)w;
                    else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                        break;
                }
            }
        }
        fclose(fp);
        printf("  [ctl  ] GET %s (%ld bytes)\n", arg, len);
    } else if (!strcmp(line, "DEL") && arg && ctl_safe(arg)) {
        char path[160];
        snprintf(path, sizeof path, CTL_DIR "/%s", arg);
        ctl_say(remove(path) == 0 ? "OK\n" : "ERR remove\n");
        printf("  [ctl  ] DEL %s\n", arg);
    } else if (!strcmp(line, "PUT") && arg) {
        /* Subdirectories are allowed now, so they may need creating. */
        char *sp = strchr(arg, ' ');
        int len = 0;
        if (sp) { *sp++ = 0; len = atoi(sp); }
        if (!ctl_safe(arg) || len < 0 || len > 32 * 1024 * 1024) {
            ctl_say("ERR bad put\n");
            return;
        }
        {
            char path[224];
            snprintf(path, sizeof path, CTL_DIR "/%s", arg);
            ctl_mkparents(arg);
            ctl_put_f = fopen(path, "wb");
            if (!ctl_put_f) { ctl_say("ERR open\n"); return; }
            ctl_put_left = len;
            printf("  [ctl  ] PUT %s (%d bytes)\n", arg, len);
            if (!len) { fclose(ctl_put_f); ctl_put_f = NULL; ctl_say("OK\n"); }
        }
    } else if (!strcmp(line, "BYE")) {
        ctl_say("OK\n");
        ctl_drop();
    } else {
        ctl_say("ERR unknown\n");
    }
}

static void ctl_poll(void) {
    if (ctl_listen < 0)
        return;
    if (ctl_client < 0) {
        int c = accept(ctl_listen, NULL, NULL);
        if (c < 0)
            return;
        fcntl(c, F_SETFL, O_NONBLOCK);
        ctl_client = c;
        ctl_line_n = 0;
        ctl_put_left = 0;
        ctl_say("BOZ ctl ready\n");
        printf("  [ctl  ] client connected\n");
    }
    for (;;) {
        if (ctl_put_left > 0) {
            char b[8192];   /* big enough that a multi-MB NRO drains in a few polls */
            int want = ctl_put_left < (int)sizeof b ? ctl_put_left : (int)sizeof b;
            int n = recv(ctl_client, b, (size_t)want, 0);
            if (n <= 0) { if (!n) ctl_drop(); return; }
            if (ctl_put_f)
                fwrite(b, 1, (size_t)n, ctl_put_f);
            ctl_put_left -= n;
            if (!ctl_put_left) {
                if (ctl_put_f) { fclose(ctl_put_f); ctl_put_f = NULL; }
                ctl_say("OK\n");
            }
            continue;
        }
        {
            char c;
            int n = recv(ctl_client, &c, 1, 0);
            if (n <= 0) { if (!n) ctl_drop(); return; }
            if (c == '\r')
                continue;
            if (c != '\n') {
                if (ctl_line_n < (int)sizeof ctl_line - 1)
                    ctl_line[ctl_line_n++] = c;
                continue;
            }
            ctl_line[ctl_line_n] = 0;
            ctl_line_n = 0;
            ctl_command(ctl_line);
            if (ctl_client < 0)
                return;
        }
    }
}

/* ---- the control socket gets its own thread ---------------------------
 *
 * ctl_poll() used to be called from the guest loop, between 5M-instruction
 * chunks, and from the post-run wait loop. That works right up until the
 * moment it is most needed: when the guest hangs INSIDE guest_run -- one call
 * that never returns, which is exactly what a bad translation does -- neither
 * caller runs again and the card becomes unreachable. Every hang today was
 * diagnosed from the outside, by watching the log stop and inferring from a
 * stalled counter, because there was no way to look in.
 *
 * On its own thread the socket survives whatever the guest is doing. The
 * guest is single-threaded by construction and cores 1 and 2 sit idle, so
 * this costs nothing that was being used.
 *
 * Deliberately the ONLY caller of ctl_poll now: two threads polling one
 * non-blocking socket would race on ctl_line and the partial-PUT state for no
 * benefit. The filesystem is still shared with the guest -- a PUT lands while
 * the guest may be reading -- but that was already true and this is a debug
 * channel, not a transaction log.
 *
 * 20 ms between polls: fast enough to feel immediate, slow enough that the
 * thread is invisible next to a guest running millions of instructions a
 * second. */
static Thread g_ctl_thread;
static int    g_ctl_thread_live;

static void ctl_thread_fn(void *arg) {
    (void)arg;
    while (g_ctl_thread_live) {
        ctl_poll();
        svcSleepThread(20000000ull);          /* 20 ms */
    }
}

static void ctl_thread_start(void) {
    Result rc;
    if (ctl_listen < 0)
        return;
    g_ctl_thread_live = 1;
    /* Core 1: the guest owns core 0 and the scheduler leaves 3 to the system.
     * Priority below the main thread so it can never delay the guest --
     * it only needs to run when the socket has something, and 20 ms of
     * latency on a debug channel is not worth a single guest stall. */
    rc = threadCreate(&g_ctl_thread, ctl_thread_fn, NULL, NULL, 0x8000, 0x3B, 1);
    if (R_FAILED(rc)) {
        /* Fall back to whatever core the process is allowed, then to the old
         * behaviour if even that fails -- an unreachable card is a nuisance,
         * a failed launch is not acceptable. */
        rc = threadCreate(&g_ctl_thread, ctl_thread_fn, NULL, NULL, 0x8000, 0x3B, -2);
    }
    if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadStart(&g_ctl_thread))) {
        printf("ctl: polling on its own thread (survives a guest hang)\n");
        return;
    }
    g_ctl_thread_live = 0;
    printf("ctl: thread failed; polling from the guest loop as before\n");
}

static void ctl_init(void) {
    struct sockaddr_in a;
    int one = 1;

    ctl_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (ctl_listen < 0)
        return;
    setsockopt(ctl_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(CTL_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ctl_listen, (struct sockaddr *)&a, sizeof a) < 0 ||
        listen(ctl_listen, 1) < 0) {
        close(ctl_listen);
        ctl_listen = -1;
        printf("ctl: listen failed; no remote control this run\n");
        return;
    }
    fcntl(ctl_listen, F_SETFL, O_NONBLOCK);
    printf("ctl: port %d open -- LS / GET name / PUT name len / DEL name,"
           " under %s\n", CTL_PORT, CTL_DIR);
    ctl_thread_start();
}

/* ------------------------------------------------------- clock readout ---
 *
 * Nothing in this log has ever recorded the clock a run was taken at. Every
 * number measured so far therefore carries an unrecorded variable: had an OC
 * profile changed between two runs, nothing would have shown it, and the pair
 * would have looked like a result.
 *
 * Recording the clock turns "was this run at stock?" from a memory into a
 * fact in the log.
 *
 * READ-ONLY, deliberately. Setting clocks belongs to the OC sysmodule, which
 * reapplies its own profile and would simply overwrite anything set here.
 *
 * Caveat worth knowing before trusting these numbers: some OC solutions patch
 * clocks below this interface, in which case clkrst reports the stock table
 * rather than the rate actually being run. Check the figure against what the
 * overlay says once; if they agree, it is trustworthy from then on. */
static void report_clocks(const char *when) {
    static const struct {
        PcvModuleId id;
        const char *name;
    } mods[] = {
        { PcvModuleId_CpuBus, "cpu" },
        { PcvModuleId_GPU,    "gpu" },
        { PcvModuleId_EMC,    "emc" },
    };
    unsigned i;

    if (R_FAILED(clkrstInitialize())) {
        printf("clocks (%s): clkrst unavailable\n", when);
        return;
    }
    printf("clocks (%s):", when);
    for (i = 0; i < 3; i++) {
        ClkrstSession sess;
        u32 hz = 0;
        if (R_SUCCEEDED(clkrstOpenSession(&sess, mods[i].id, 3))) {
            if (R_SUCCEEDED(clkrstGetClockRate(&sess, &hz)))
                printf(" %s %u MHz", mods[i].name, (unsigned)(hz / 1000000u));
            else
                printf(" %s ?", mods[i].name);
            clkrstCloseSession(&sess);
        } else {
            printf(" %s (no session)", mods[i].name);
        }
    }
    printf("\n");
    clkrstExit();
}

static void run(void) {
    static const char *paths[] = {"sdmc:/switch/boz/boz.s3e.unpacked",
                                  "sdmc:/boz.s3e.unpacked"};
    size_t size = 0;
    unsigned char *file = NULL;
    uint32_t n, i;
    GuestStatus st;

    startup_stage_write("01 run entered");
    advanced_report();

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
    if (s3e_load(file, size, IMAGE_BASE, &g_img) != 0) {
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
        /* Optional extra keys from the card, applied on top of the game
         * own ones. The only way to ask this engine a question is to change a
         * config value and watch what happens, and doing that through a
         * rebuild-and-deploy cycle per key turns an experiment into an
         * afternoon. Absent by default, so it costs nothing.
         *
         * The buffer is deliberately never freed: config entries point into
         * it rather than copying, so it has to outlive them. */
        {
            /* Play Online keys first, then the card's override.icf on top --
             * one blob, because the config layer takes overrides only once.
             * GENERIC is the account type the community server logs in, and
             * 1.0.11 is the version the PS Vita and PortMaster clients report:
             * rooms are matched on it, so it is what puts us in theirs. */
            static const char online[] =
                "[GAME]\nOnlineAccount=GENERIC\nGameVersion=1.0.11\n"
                "VoiceChatEnabled=0\n";
            char *text = NULL;
            size_t tlen = 0;
#ifdef __SWITCH__
            if (net_online_enabled()) {
                text = (char *)malloc(sizeof online);
                if (text) {
                    memcpy(text, online, sizeof online - 1);
                    tlen = sizeof online - 1;
                }
            }
#endif
            {
                FILE *cf = fopen("sdmc:/switch/boz/override.icf", "rb");
                if (cf) {
                    long clen;
                    fseek(cf, 0, SEEK_END);
                    clen = ftell(cf);
                    fseek(cf, 0, SEEK_SET);
                    if (clen > 0 && clen < (1 << 20)) {
                        char *grown = (char *)realloc(text, tlen + (size_t)clen + 2u);
                        if (grown) {
                            text = grown;
                            text[tlen++] = '\n';
                            if (fread(text + tlen, 1, (size_t)clen, cf) == (size_t)clen)
                                tlen += (size_t)clen;
                        }
                    }
                    fclose(cf);
                }
            }
            if (tlen)
                printf("cfg: overrides applied (online %s), %d keys total\n",
#ifdef __SWITCH__
                       net_online_enabled() ? "on" : "off",
#else
                       "off",
#endif
                       s3e_config_load_overrides(text, (unsigned)tlen));
            else
                free(text);
        }
    }
    /* On-screen touch markers and pointer logging. */
    g_touch_dbg = adv_int("touch_debug", 0);
    startup_stage_write("04 VFS and GL allocator ready");

    g_stack = page_alloc(STACK_SIZE);
    g_heap = page_alloc(HEAP_SIZE);
    g_surf = page_alloc(SURF_BYTES);
    if (!g_stack || !g_heap || !g_surf) {
        printf("out of memory\n");
        return;
    }
    startup_stage_write("05 guest buffers allocated");
    guest_mem_add(&g.mem, g_img.load_base, g_img.image_alloc, g_img.image, 1);
    guest_mem_add(&g.mem, STACK_BASE, STACK_SIZE, g_stack, 1);
    guest_mem_add(&g.mem, HEAP_BASE, HEAP_SIZE, g_heap, 1);
    guest_mem_add(&g.mem, SURF_BASE, SURF_BYTES, g_surf, 1);
    guest_mem_add(&g.mem, SCRATCH_BASE, SCRATCH_SIZE, g_scratch, 1);
    g_memp = &g.mem;
    startup_stage_write("06 guest memory mapped");
#ifdef __SWITCH__
    {   /* Player name. The game builds its online name with
         * sprintf(name, "Player-%d", n) through one PC-relative literal
         * (RVA 0x18f74c, used by the add-pc at 0x18f606). Repointing that
         * literal at a plain string makes the sprintf produce it verbatim --
         * the same patch the PortMaster port applies, at the same addresses,
         * checked byte for byte before anything is written. */
        const uint32_t lit_at = g_img.load_base + 0x18f74cu;
        const uint32_t pc_at  = g_img.load_base + 0x18f60au;
        const uint32_t fmt_at = g_img.load_base + 0x3af131u;
        const char *fmt = (const char *)guest_ptr(&g.mem, fmt_at, 10);
        uint32_t lit = 0;
        if (guest_ld32(&g.mem, lit_at, &lit) && lit == fmt_at - pc_at && fmt &&
            !memcmp(fmt, "Player-%d", 10)) {
            const char *name = net_player_name();
            const uint32_t at = galloc((uint32_t)strlen(name) + 1u);
            if (at && gputs(&g.mem, at, name)) {
                guest_st32(&g.mem, lit_at, at - pc_at);
                printf("  [net  ] player name: %s\n", name);
            }
        } else {
            printf("  [net  ] player name reference not found; keeping Player-N\n");
        }
    }
#endif

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
    /* Five more spares for the s3eTouchpad table. The game CALLS these, so
     * each needs its own guest address rather than sharing the ext stub. */
    {
        static const struct { const char *nm; GuestHleFn fn; } tp[5] = {
            { "<s3eTouchpadRegister>",   hle_tp_register   },
            { "<s3eTouchpadUnRegister>", hle_tp_unregister },
            { "<s3eTouchpad spare0>",    hle_zero          },
            { "<s3eTouchpad spare1>",    hle_zero          },
            { "<s3eTouchpadGetInt>",     hle_tp_getint     },
        };
        unsigned k;
        for (k = 0; k < 5 && n + 1 + (int)k < 512; k++) {
            g_slots[n + 1 + k].name = tp[k].nm;
            g_slots[n + 1 + k].fn = tp[k].fn;
            g_tp_stub[k] = GUEST_STUB_BASE + 4 * (n + 1 + (int)k);
        }
    }
#ifdef __SWITCH__
    /* And five for s3eZeroConf, right after them. */
    {
        static const char *zc_names[5] = {
            "<s3eZeroConfStartSearch>", "<s3eZeroConfStopSearch>",
            "<s3eZeroConfPublish>", "<s3eZeroConfUpdateTxtRecord>",
            "<s3eZeroConfUnpublish>",
        };
        unsigned k;
        for (k = 0; k < 5 && n + 6 + (int)k < 512; k++) {
            g_slots[n + 6 + k].name = zc_names[k];
            g_slots[n + 6 + k].fn = net_zeroconf_fn(k);
            g_zc_stub[k] = GUEST_STUB_BASE + 4 * (n + 6 + (int)k);
        }
    }
#endif
    /* The HLE profiler: splits frame time between guest code and the
     * handlers. Opt-in, because the dynarmic boundary times itself with four
     * CNTPCT_EL0 reads per import call (~15 600 a frame here) and tests this
     * pointer to decide whether to. */
    if (adv_int("profilers", 0)) {
        g.prof = hle_profile;
        g_prof_on = 1;
        runtime_note("profilers");
        printf("profilers on\n");
    }

    /* Real time, or the old fixed 16 ms step; see hle_timer_ms. Real time
     * is the default -- the fixed step made game speed a function of frame
     * rate. fixed_clock puts the reproducible clock back, which is what a
     * benchmark pair needs so both arms run the same guest work. */
    {
        g_clock_fixed = adv_int("fixed_clock", 0);
        printf("clock %s\n", g_clock_fixed
               ? "fixed 16 ms/query" : "real time");
        if (g_clock_fixed)
            runtime_note("fixed clock");
    }

    /* dynarmic_cache_mb sets the code cache. The default is far below
     * dynarmic's own 128 MB: this is one game, the translations are bounded by
     * how much of the image actually runs, and on this console that memory is
     * taken from a heap the guest also needs. */
    {
        const int mb = adv_int("dynarmic_cache_mb", (int)g_dyn_mb);
        if (mb >= 4 && mb <= 256)
            g_dyn_mb = (unsigned)mb;
    }

    /* Benchmark mode; see the comment on g_bench. Deliberately NOT combined
     * with anything else -- the point of the run is that one thing differs
     * between it and its pair, so the other switches stay where they are. */
    {
        const int million = adv_int("bench_million", 0);
        g_bench = adv_int("bench", 0);
        if (million > 0)
            g_bench_target = (uint64_t)million * 1000000ull;
        if (g_bench) {
            if (!g_bench_target)
                g_bench_target = BENCH_INSTR;
            printf("bench on: stopping at %lluM instructions,"
                   " real input suppressed\n",
                   (unsigned long long)(g_bench_target / 1000000ull));
            runtime_note("BENCHMARK (input off)");
        }
    }

    report_clocks("start");

    g.hle.slot = g_slots;
    /* imports, the ext stub, and the five s3eTouchpad table entries.
     * The dispatcher bounds-checks against this, so a slot past it is
     * not called at all and the guest returns into nothing -- which is
     * a black screen about four seconds in. */
#ifdef __SWITCH__
    g.hle.count = n + 1 + 5 + 5;   /* ... and the five s3eZeroConf entries */
#else
    g.hle.count = n + 1 + 5;
#endif
    /* Sound output. Failure is not fatal: every snd_out_* call becomes a
     * no-op and the HLE handlers keep reporting the working-but-idle device
     * they reported before there was any output at all. */
    g_snd_live = snd_out_init();
    if (g_snd_live)
        printf("  [snd  ] audout up: 48 kHz stereo, %d voices\n", SND_OUT_CHANNELS);
    else
        printf("  [snd  ] no audio output; sound stays silent\n");
    startup_stage_write("07 imports bound");

    /* Native hooks. None of these is a speed-up for guest code -- dynarmic
     * compiles that itself, and an interception costs it a block exit and a
     * register sync each way. What remains is what changes behaviour:
     *
     *   malloc / realloc / free  stand in for an allocator whose vtable this
     *                            image never populates
     *   memcpy / memset          amortised over the bytes they move
     *   image handler repair     restores a registry handler's NULL vtable
     *   angle normalise          fmodf instead of a subtract loop that ran
     *                            ~44M iterations on one bad value; skipping
     *                            it hangs the game */
    static GuestHook hooks[8];
    hooks[0].addr = g_img.load_base + RVA_MGR_MALLOC;
    hooks[0].fn = hook_malloc;
    hooks[1].addr = g_img.load_base + RVA_MGR_REALLOC;
    hooks[1].fn = hook_realloc;
    hooks[2].addr = g_img.load_base + RVA_MGR_FREE;
    hooks[2].fn = hook_free;
    hooks[3].addr = g_img.load_base + RVA_IMAGE_HANDLER_READY;
    hooks[3].fn = repair_image_handler;
    hooks[3].observe = 1;
    hooks[4].addr = g_img.load_base + RVA_FAST_ANGLE_NORMALIZE;
    hooks[4].fn = fast_angle_normalize;
    hooks[4].observe = 1;
    hooks[5].addr = g_img.load_base + RVA_NATIVE_MEMCPY;
    hooks[5].fn = hook_native_memcpy;
    hooks[6].addr = g_img.load_base + RVA_NATIVE_MEMSET;
    hooks[6].fn = hook_native_memset;
    g.hook = hooks;
    g.hook_count = 7;

    g.cpu.r[GUEST_SP] = STACK_BASE + STACK_SIZE - 16;
    g.cpu.cpsr = CPSR_Z;   /* Unicorn's reset state; flags are undefined
                            * at entry, so match the reference harness. */
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
    /* After the regions AND the hooks: the page table is built from the
     * region list, and the interception filter from the hook table, so
     * anything registered later would be invisible to one or the other. */
    /* Failure is not fatal: executable memory depends on how the homebrew
     * was launched, so dyn_init reports and returns 0, and dyn_run carries on
     * with the interpreter -- slowly, but correctly. */
    g_dyn_live = dyn_init(&g, g_dyn_mb);
    if (!g_dyn_live)
        printf("cpu: dynarmic unavailable, falling back to the interpreter\n");
    {   /* Only now is the engine actually known: dynarmic asks the kernel for
         * executable memory and does not always get it. */
        char note[48];
        if (g_dyn_live)
            snprintf(note, sizeof note, "dynarmic %u MB", g_dyn_mb);
        else
            snprintf(note, sizeof note, "interpreter (dynarmic failed)");
        runtime_note(note);
    }
    startup_stage_write("09 entering guest code");
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
            st = dyn_run(&g, 0xFFFFFFFFu, delta[probe]);
            if (st != GUEST_STEP_LIMIT)
                break;
            startup_stage_write(done[probe]);
        }
        if (st == GUEST_STEP_LIMIT) {
            for (probe = 0; probe < 40; probe++) {
                char detail[96];
                st = dyn_run(&g, 0xFFFFFFFFu, 100000);
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
    g_bench_t0 = armGetSystemTick();
    uint64_t quit_held = 0;   /* tick the escape combo went held, 0 if not */
    int stage_cleared = 0;    /* the breadcrumb, dropped once we are live */
    while (!g_quit && st == GUEST_STEP_LIMIT) {
        st = dyn_run(&g, 0xFFFFFFFFu, 5000000ull);
        /* Sixty-odd frames on screen is past every early death the
         * breadcrumb exists to catch. */
        if (!stage_cleared && g_presents > 60) {
            startup_stage_clear();
            stage_cleared = 1;
        }
        /* Stop the CLOCK at the target, but not the run: EGL owns the
         * window, so breaking out here leaves whatever frame happened to
         * be on screen sitting there forever, and a benchmark that ends
         * mid-load is indistinguishable from a hang. It read as one.
         * Keep presenting for a moment so the banner in fps_overlay
         * actually reaches the panel. */
        if (g_bench && !g_bench_done && g.executed >= g_bench_target) {
            uint64_t freq = armGetSystemTickFreq();
            g_bench_ms = (armGetSystemTick() - g_bench_t0) * 1000ull / freq;
            g_bench_instr = g.executed;
            g_bench_presents = g_presents;
            g_bench_done = 1;
            g_bench_until = g_presents + 90;
        }
        /* The frame guard is not the only exit: a run that stops
         * presenting would otherwise never reach it. */
        if (g_bench_done && (g_presents >= g_bench_until
                             || g.executed > g_bench_instr + 300000000ull)) {
            uint64_t r10 = g_bench_ms
                         ? g_bench_instr / (g_bench_ms * 100ull) : 0;
            report_clocks("end");
            printf("\n[bench] %lluM instructions in %llu ms = %llu.%llu M/s,"
                   " %d presents\n",
                   (unsigned long long)(g_bench_instr / 1000000ull),
                   (unsigned long long)g_bench_ms,
                   (unsigned long long)(r10 / 10ull),
                   (unsigned long long)(r10 % 10ull), g_bench_presents);
            break;
        }
        if (st != GUEST_STEP_LIMIT)
            break;
        if (!appletMainLoop())
            break;
        /* The harness escape hatch. A single press of + was fine while this
         * was only ever a benchmark and nothing else read the pad. Now + is
         * the game's pause key, so that binding did two things at once: the
         * press opened settings AND stopped the run, and the next press hit
         * "Press + to exit" below and dropped to HorizonOS. On repeated
         * presses it looked exactly like a crash.
         *
         * It was also intermittent, which is what sent the diagnosis after
         * the key encoding instead. padGetButtonsDown reports the delta since
         * the LAST padUpdate, and key_update and pad_touch_update each call
         * one per frame -- so whichever ran first usually ate the edge and
         * this check saw nothing. Pressing + repeatedly was buying tickets in
         * that race.
         *
         * So: a button the game does not use, held for a second, and read
         * from the held state rather than an edge, which cannot race. */
        padUpdate(&g_pad);
        /* MINUS ALONE IS NOT ENOUGH. It was, and a long press on it during
         * play stopped the game dead on the "Press + to exit" screen -- which
         * from the player's seat is indistinguishable from a freeze. Minus is
         * a button someone holds by accident; Minus AND Plus together for a
         * second is not. Still read from the held state rather than an edge,
         * which is what made this reliable in the first place. */
        {
            const u64 escape = HidNpadButton_Minus | HidNpadButton_Plus;
            if ((padGetButtons(&g_pad) & escape) == escape) {
                if (!quit_held)
                    quit_held = armGetSystemTick();
                else if (armGetSystemTick() - quit_held > armGetSystemTickFreq()) {
                    printf("  ... stopped by - and + held\n");
                    break;
                }
            } else {
                quit_held = 0;
            }
        }
        printf("  ... %lluM instructions, pc=%06x, %d presents\n",
               (unsigned long long)(g.executed / 1000000ull),
               (unsigned)(g.cpu.r[15] - g_img.load_base), g_presents);
        instr_profile_report(g.executed, g_presents);
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
    printf("native: memcpy %u calls/%llu bytes, memset %u calls/%llu bytes\n",
           (unsigned)g_fast_mem_hits[0],
           (unsigned long long)g_fast_mem_bytes[0],
           (unsigned)g_fast_mem_hits[1],
           (unsigned long long)g_fast_mem_bytes[1]);
}

/* Launched from hbmenu rather than by nxlink, __nxlink_host is zero and
 * nxlinkStdio() has nothing to connect to -- it returns -1 and the whole log
 * is stranded on a console that the framebuffer takes over at the first frame.
 * The address is just a global, so read it off the card instead. Put the PC's
 * IPv4 address in the nxlink_host setting and run a listener on port 28771
 * (NXLINK_CLIENT_PORT); the stream is plain text with no handshake.
 *
 * This removes netloader from the loop entirely, which matters because
 * netloading writes the NRO to the SD, and that write is what has been
 * failing. */
/* Is anything actually listening on the nxlink host?
 *
 * nxlinkConnectToHost does a BLOCKING connect to __nxlink_host:28771, on the
 * main thread, before run() is ever reached. That is fine when the PC is
 * there. With the PC off -- which is the normal state for simply PLAYING the
 * game -- it blocks for the full TCP SYN timeout and the game looks like it
 * does not start at all. A configured nxlink_host makes that the
 * guaranteed path rather than a rare one, because it is what supplies
 * an address to hang on; without it __nxlink_host is zero and the call fails
 * immediately, which is why this never showed up while netloading.
 *
 * So probe first, non-blocking, with a deadline a host on the same LAN beats
 * by three orders of magnitude, and only hand over if the probe connects. */
static int nxlink_reachable(void) {
    struct sockaddr_in sa;
    struct timeval tv;
    fd_set wr;
    socklen_t len = sizeof(int);
    int fd, flags, err = 0, ret;

    if (!__nxlink_host.s_addr)
        return 0;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(NXLINK_CLIENT_PORT);
    sa.sin_addr = __nxlink_host;
    ret = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (ret == 0) {
        close(fd);
        return 1;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return 0;
    }
    FD_ZERO(&wr);
    FD_SET(fd, &wr);
    tv.tv_sec = 0;
    tv.tv_usec = 400000;              /* a host on the LAN answers in ~1 ms */
    ret = select(fd + 1, NULL, &wr, NULL, &tv);
    if (ret > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && !err) {
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

/* The development host that stdout is streamed to when the game was not
 * launched by nxlink. A config.txt key like every other switch; the old
 * nxlink_host.txt was imported into it at startup. */
static int nxlink_host_from_file(void) {
    const char *host = settings_get("nxlink_host", NULL);
    if (!host || !host[0]) {
        printf("no nxlink_host in config.txt\n");
        return 0;
    }
    if (inet_pton(AF_INET, host, &__nxlink_host) == 1) {
        printf("nxlink host %s (config.txt)\n", host);
        return 1;
    }
    printf("bad nxlink_host \"%s\" in config.txt\n", host);
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

    {   /* A breadcrumb still on the card means the last launch died before it
         * finished starting -- which is also what puts this run in safe mode,
         * so the settings that could have caused it are ignored once. */
        const int died = startup_stage_read(previous_stage, sizeof previous_stage);
        if (died) {
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
        /* Before anything reads a setting, nxlink's host included. */
        advanced_init(died);
    }

    printf("Call of Duty: Black Ops Zombies - %s\n\nconnecting to nxlink host...\n",
           BOZ_BUILD_LABEL);
    consoleUpdate(NULL);
    if (R_SUCCEEDED(socketInitializeDefault())) {
        sockets_up = 1;
        ctl_init();
        /* Probe before each connect, so an absent host costs 400 ms once
         * instead of a full TCP timeout twice. */
        if (nxlink_reachable())
            nxfd = nxlinkStdio();
        if (nxfd < 0) {                 /* not netloaded: try the card */
            consoleUpdate(NULL);
            if (nxlink_host_from_file() && nxlink_reachable())
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
    {   /* after sockets: Play Online needs them, and reads config.txt */
        static const NetGlue glue = { net_glue_alloc, net_glue_call3 };
        net_init(&glue, sockets_up);
        port_settings_apply();
    }
    /* Whether the console is still in the output path decides if it is
     * safe to print after the window changes hands. */
    g_nxlink_up = (nxfd >= 0);
    setvbuf(stdout, NULL, _IONBF, 0);   /* stream lines as they happen */

    printf("Call of Duty: Black Ops Zombies - %s\n\n", BOZ_BUILD_LABEL);
    run();
    printf("\nPress + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus)
            break;
        /* ctl_poll now runs on its own thread; see ctl_thread_start. */
        consoleUpdate(NULL);
    }
    s3e_vfs_sync();             /* saves still queued for the card */
    if (nxfd >= 0)
        close(nxfd);
    if (sockets_up)
        socketExit();
    consoleExit(NULL);
    return 0;
}
