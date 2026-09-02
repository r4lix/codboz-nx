/* Interpreter bring-up test.
 *
 * Loads the image, binds every GOT slot to the stub page, and interprets from
 * the ARM entry stub. The HLE set here is the minimum the Unicorn reference
 * (../../loader/run_boz.py) showed is needed to get through early startup.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <stdint.h>

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
static struct { uint32_t addr, size; } *g_allocs;
static uint32_t g_alloc_n, g_alloc_cap;

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
    g_alloc_n++;
}

/* 0 for a pointer this allocator never handed out -- the reference treats an
 * unknown pointer the same way and copies nothing. */
static uint32_t galloc_size(uint32_t addr) {
    uint32_t lo = 0, hi = g_alloc_n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (g_allocs[mid].addr == addr)
            return g_allocs[mid].size;
        if (g_allocs[mid].addr < addr)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return 0;
}

static uint32_t galloc(uint32_t n) {
    uint32_t p;
    n = (n + 15u) & ~15u;
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
    return p;
}

/* realloc has to PRESERVE the old contents. Returning a fresh block without
 * copying silently zeroes whatever was there; the game grows arrays of object
 * pointers through here, so every growth left holes, and a later linear scan
 * dereferenced a null entry before reaching the element count. */
static uint32_t grealloc(GuestMem *mem, uint32_t old, uint32_t n) {
    uint32_t p = galloc(n);
    uint32_t keep = galloc_size(old);
    if (p && old && keep) {
        void *dst, *src;
        if (keep > n)
            keep = n;
        dst = guest_ptr(mem, p, keep);
        src = guest_ptr(mem, old, keep);
        if (dst && src)
            memcpy(dst, src, keep);
    }
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
    cpu->r[0] = 0;
}

/* Observe-only: report what memset is actually asked to do. The question this
 * has to settle is whether the length is already wrong on entry (a bug in the
 * caller, i.e. upstream execution) or the fill loop overruns a correct one. */
#define RVA_MEMSET 0x3664f4u
static int g_memsets;
static uint32_t g_ms_dst, g_ms_val, g_ms_len, g_ms_lr;
static void watch_memset(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)mem; (void)user;
    g_ms_dst = cpu->r[0];
    g_ms_val = cpu->r[1];
    g_ms_len = cpu->r[2];
    g_ms_lr  = cpu->r[GUEST_LR];
    /* every early call, then only the implausible ones */
    if (g_memsets < 24 || cpu->r[2] >= 0x10000u)
        printf("  [memset %d] dst=%08x val=%02x len=%x lr=%06x\n",
               g_memsets, (unsigned)cpu->r[0], (unsigned)(cpu->r[1] & 0xff),
               (unsigned)cpu->r[2],
               (unsigned)(cpu->r[GUEST_LR] - g_img.load_base));
    g_memsets++;
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
    {   /* Who chose this name? The .dz variant is picked in code, and the LR
         * at the open call names the chooser. */
        static int dz_shown;
        size_t l = strlen(fn);
        if (l > 3 && !strcmp(fn + l - 3, ".dz") && dz_shown < 4) {
            dz_shown++;
            printf("  [file ] .dz request %s from lr=%08x (RVA %06x)\n", fn,
                   (unsigned)cpu->r[14], (unsigned)(cpu->r[14] - 0x4a000000u));
        }
    }
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
/* A deep differential cannot align the two harnesses by instruction index:
 * Unicorn does not fire UC_HOOK_CODE for an IT-block instruction whose
 * condition fails, and this interpreter steps (and counts) it, so the two
 * counters drift apart by a growing amount -- at a billion instructions the
 * skew dwarfs any window worth comparing. So the window is anchored on a
 * *landmark* both sides count natively and identically instead: the number of
 * s3eSurfaceShow calls. BOZ_TRACE_AT=<present#> opens the trace there,
 * BOZ_TRACE_COUNT=<n> closes it n instructions later and ends the run. */
static uint64_t g_steps;            /* global instruction index */
static uint64_t g_trace_at;         /* 0 = trace from the first instruction */
static uint64_t g_trace_count;
static uint64_t g_trace_written;
static int g_tracing;               /* window currently open */
static uint64_t g_window_step;      /* index the window opened at */

/* BOZ_WATCH=<guest addr>: log every change of one 32-bit word, as
 * {step, pc, value}. The state hash covers registers only, so a store that
 * goes to the wrong address -- or is skipped -- is invisible until something
 * loads the word back, which can be hundreds of millions of instructions
 * later. Diffing the two harnesses' change logs for the word names the store.
 * A polled compare rather than a real watchpoint: one aligned load per step,
 * which costs a few percent and needs no hooks in the interpreter core. */
/* BOZ_TRACE_PC=<addr> + BOZ_TRACE_NTH=<n>: open the window the n'th time that
 * address executes. A present is too coarse an anchor once a memory watch has
 * named the call that goes wrong -- this puts the window straight on it, and
 * the anchored instruction is record 0. */
static uint32_t g_trace_pc;
static uint64_t g_trace_nth, g_pc_hits;

static uint32_t g_watch_addr, g_watch_last;
static FILE *g_watch_f;
static uint64_t g_watch_n;

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
static uint32_t g_ev_press, g_ev_release;
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
    printf("  [tap  ] queueing pointer callback pressed=%d at (%d,%d)\n",
           pressed, g_tap_x, g_tap_y);
    if (!cb_queue("s3ePointer", 0, *buf))
        printf("  [tap  ] no s3ePointer id=0 callback registered\n");
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

/* void s3ePointerUpdate(void) -- verified: at its only call site (RVA
 * 0x0c6506) r0 is overwritten by `ldr r0,[r3]` immediately after, so the
 * return value is discarded. */
static void hle_ptr_update(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    if (g_tap_armed && !g_tap_done) {
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
static void surf_dump(int n) {
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

static void hle_surface_show(GuestCpu *cpu, GuestMem *mem, void *user) {
    (void)user;
    g_presents++;
    if (g_trace_at && !g_tracing && !g_window_step &&
        (uint64_t)g_presents == g_trace_at) {
        g_tracing = 1;
        g_window_step = g_steps ? g_steps : 1;
        printf("  [diff ] trace window opens at present #%d, step %llu\n",
               g_presents, (unsigned long long)g_steps);
    }
    tap_arm(g_presents);
    surf_find_source(g_presents);
    /* Dump densely around the tap so before/after can be compared directly. */
    if (g_presents <= 4 || (g_presents % 100) == 0 ||
        (g_tap_frame > 0 && g_presents >= g_tap_frame - 2 &&
         g_presents <= g_tap_frame + 30)) {
        printf("  [surf ] present #%d hash=%08x step=%llu\n", g_presents,
               (unsigned)surf_hash(), (unsigned long long)g_steps);
        surf_dump(g_presents);
    } else if ((g_presents % 25) == 0) {
        printf("  [surf ] present #%d hash=%08x step=%llu\n", g_presents,
               (unsigned)surf_hash(), (unsigned long long)g_steps);
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

/* s3eMemoryGetInt(prop). Unbound, this fell to hle_default and returned 0 --
 * and a free-memory figure of zero is why the game opens with a "DEVICE IS LOW
 * ON RAM / switch off and restart your device" dialog before the menu. The
 * answers come from the real bump allocator so they stay consistent with each
 * other and with what the game can actually get.
 *
 * Marmalade's property order is not documented in anything shipped here, so
 * each id is logged on first sight and the default is a LARGE value: zero is
 * the one answer known to trigger the warning, so it must never be the
 * fallback. */
static void hle_memory_getint(GuestCpu *cpu, GuestMem *mem, void *user) {
    static uint32_t seen;
    uint32_t prop = cpu->r[0];
    uint32_t used = g_brk - HEAP_BASE;
    uint32_t avail = HEAP_SIZE - used;
    uint32_t v;
    (void)mem; (void)user;
    switch (prop) {
    case 0:  v = HEAP_SIZE; break;      /* total heap */
    case 1:  v = used;      break;      /* used */
    case 2:  v = avail;     break;      /* free */
    case 3:  v = avail;     break;      /* largest free block */
    case 4:  v = avail;     break;      /* lowest free ever */
    case 5:  v = used;      break;      /* highest used ever */
    default: v = avail;     break;
    }
    if (prop < 32 && !(seen & (1u << prop))) {
        seen |= 1u << prop;
        printf("  [mem  ] GetInt(%u) -> %u (%u MB used of %u MB)\n",
               (unsigned)prop, (unsigned)v, (unsigned)(used >> 20),
               (unsigned)(HEAP_SIZE >> 20));
    }
    cpu->r[0] = v;
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
         * usually "what was it asked for", and for the EGL entry points it is
         * the whole question -- eglGetDisplay(0) means the guest already asks
         * for EGL_DEFAULT_DISPLAY and the failure is downstream. */
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
    else if (!strcmp(nm, "s3eExtGetHash"))
        g_slots[i].fn = hle_extgethash;
    else if (!strcmp(nm, "s3eDeviceGetString"))
        g_slots[i].fn = hle_devstring;
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


/* ------------------------------------------------------------- host driver */

/* One record per retired instruction: the PC, and a hash of the architectural
 * state that follows it. PC alone finds control-flow divergence; the hash also
 * catches a wrong *value* that has not reached a branch yet -- which is the
 * class the decode checker and the fault-driven hardware runs both miss. */
static FILE *g_trace;

static uint32_t state_hash(const GuestCpu *c) {
    uint32_t h = 2166136261u, i, b;
    uint32_t flags = c->cpsr &
        (CPSR_N | CPSR_Z | CPSR_C | CPSR_V | CPSR_T | CPSR_GE_MASK);
    for (i = 0; i < 15; i++)                 /* r15 travels separately as pc */
        for (b = 0; b < 4; b++) {
            h ^= (c->r[i] >> (8 * b)) & 0xFFu;
            h *= 16777619u;
        }
    for (b = 0; b < 4; b++) {
        h ^= (flags >> (8 * b)) & 0xFFu;
        h *= 16777619u;
    }
    /* VFP is in the hash: without it a wrong double stays invisible until it
     * moves into a core register, which is why the atan divergence surfaced
     * thousands of instructions after the instruction that caused it. */
    for (i = 0; i < 64; i++)                 /* d0-d31, not just d0-d15 */
        for (b = 0; b < 4; b++) {
            h ^= (c->s[i] >> (8 * b)) & 0xFFu;
            h *= 16777619u;
        }
    /* NZCV only: rounding mode and the cumulative exception bits are not
     * modelled and would diverge without meaning anything. */
    for (b = 0; b < 4; b++) {
        h ^= ((c->fpscr & 0xF0000000u) >> (8 * b)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

static void dump_regs(const char *tag, const GuestCpu *c) {
    unsigned i;
    printf("%s pc=%08x %s\n", tag, (unsigned)c->r[15],
           guest_is_thumb(c) ? "Thumb" : "ARM");
    for (i = 0; i < 15; i++)
        printf(" r%-2u=%08x%s", i, (unsigned)c->r[i], (i & 3) == 3 ? "\n" : "");
    printf("\ncpsr=%08x\n", (unsigned)c->cpsr);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "boz.s3e.unpacked";
    const char *tpath = argc > 2 ? argv[2] : "interp.trace";
    uint64_t limit = argc > 3 ? strtoull(argv[3], NULL, 0) : 2000000ull;
    /* Optional window of full register dumps: the trace hash localises a
     * divergence, but only the registers say which one is wrong. */
    uint64_t full_start = argc > 4 ? strtoull(argv[4], NULL, 0) : ~0ull;
    uint64_t full_count = argc > 5 ? strtoull(argv[5], NULL, 0) : 0ull;
    int bench = 0;                  /* BOZ_BENCH: throughput, not differential */
    /* Windowed differential: with BOZ_TRACE_AT set nothing is recorded until
     * the anchor present, and full_start then counts from the window rather
     * than from the entry point -- the absolute index is not knowable ahead
     * of the run. */
    {
        const char *e = getenv("BOZ_BENCH");
        bench = e && *e && strcmp(e, "0") != 0;
        e = getenv("BOZ_WATCH");
        g_watch_addr = e ? (uint32_t)strtoul(e, NULL, 0) : 0u;
        e = getenv("BOZ_TRACE_AT");
        g_trace_at = e ? strtoull(e, NULL, 0) : 0ull;
        e = getenv("BOZ_TRACE_PC");
        g_trace_pc = e ? (uint32_t)strtoul(e, NULL, 0) : 0u;
        e = getenv("BOZ_TRACE_NTH");
        g_trace_nth = e ? strtoull(e, NULL, 0) : 1ull;
        e = getenv("BOZ_TRACE_COUNT");
        g_trace_count = e ? strtoull(e, NULL, 0) : 0ull;
        g_tracing = (g_trace_at || g_trace_pc) ? 0 : 1;
    }
    size_t size = 0;
    unsigned char *file;
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: survive a hard crash */
    uint32_t n, i;
    GuestStatus st = GUEST_OK;
    uint64_t steps;

    file = slurp(path, &size);
    if (!file) {
        printf("cannot read %s\n", path);
        return 1;
    }
    if (s3e_load(file, size, 0, &g_img) != 0) {
        printf("s3e_load failed: %s\n", s3e_error());
        return 1;
    }

    {   /* the archives and boz_files.idx sit beside the image */
        char root[512];
        const char *slash = strrchr(path, '/');
        const char *bslash = strrchr(path, '\\');
        if (bslash > slash)
            slash = bslash;
        if (slash) {
            size_t k = (size_t)(slash - path);
            if (k >= sizeof root)
                k = sizeof root - 1;
            memcpy(root, path, k);
            root[k] = 0;
        } else {
            root[0] = '.';
            root[1] = 0;
        }
        printf("vfs: %d entries from %s\n", s3e_vfs_init(root), root);
        s3e_config_set_build_style(s3e_vfs_build_style());
        printf("cfg: ResBuildStyle=%s\n", s3e_config_build_style());
    }

    g_stack = calloc(1, STACK_SIZE);
    g_heap = calloc(1, HEAP_SIZE);
    g_surf = calloc(1, SURF_BYTES);
    if (!g_stack || !g_heap || !g_surf) {
        printf("out of memory\n");
        return 1;
    }
    guest_mem_add(&g.mem, g_img.load_base, g_img.image_alloc, g_img.image, 1);
    guest_mem_add(&g.mem, STACK_BASE, STACK_SIZE, g_stack, 1);
    guest_mem_add(&g.mem, HEAP_BASE, HEAP_SIZE, g_heap, 1);
    guest_mem_add(&g.mem, SURF_BASE, SURF_BYTES, g_surf, 1);

    n = g_img.got_count < 511 ? g_img.got_count : 511;
    for (i = 0; i < n; i++) {
        uint32_t stub = GUEST_STUB_BASE + 4 * i;
        bind_slot(i, slot_name(i));
        memcpy(g_img.image + g_img.got_rva[i], &stub, 4);
    }
    g_slots[n].name = "<ext stub>";
    g_slots[n].fn = hle_zero;
    g_ext_stub = GUEST_STUB_BASE + 4 * n;
    g.hle.slot = g_slots;
    g.hle.count = n + 1;

    {
        static GuestHook hooks[4];
        hooks[0].addr = g_img.load_base + RVA_MGR_MALLOC;
        hooks[0].fn = hook_malloc;
        hooks[1].addr = g_img.load_base + RVA_MGR_REALLOC;
        hooks[1].fn = hook_realloc;
        hooks[2].addr = g_img.load_base + RVA_MGR_FREE;
        hooks[2].fn = hook_free;
        hooks[3].addr = g_img.load_base + RVA_MEMSET;
        hooks[3].fn = watch_memset;
        hooks[3].observe = 1;
        g.hook = hooks;
        g.hook_count = 4;
    }

    g.cpu.r[GUEST_SP] = STACK_BASE + STACK_SIZE - 16;
    g.cpu.cpsr = CPSR_Z;   /* Unicorn's reset state; flags are undefined
                            * at entry, so match the oracle rather than
                            * leave the differential misaligned. */
    g.cpu.r[15] = g_img.entry;

    /* "-" disables tracing. A long run traces 8 bytes per instruction, so a
     * multi-billion-instruction experiment would write tens of GB for nothing
     * -- only the differential needs the trace. */
    if (strcmp(tpath, "-") == 0) {
        g_trace = NULL;
        printf("tracing disabled\n");
    } else {
        g_trace = fopen(tpath, "wb");
        if (!g_trace) {
            printf("cannot write %s\n", tpath);
            return 1;
        }
    }
    printf("image %u KB, %u slots -> %s\n", g_img.image_size >> 10,
           (unsigned)n, tpath);
    if (g_watch_addr) {
        g_watch_f = fopen("watch_c.bin", "wb");
        guest_ld32(&g.mem, g_watch_addr, &g_watch_last);
        printf("watching %08x (initially %08x) -> watch_c.bin\n",
               (unsigned)g_watch_addr, (unsigned)g_watch_last);
    }
    if (g_trace_at)
        printf("diff window: %llu instructions from present #%llu\n",
               (unsigned long long)g_trace_count,
               (unsigned long long)g_trace_at);

    /* BOZ_BENCH: throughput mode. The loop below single-steps and hashes every
     * instruction -- that is the differential's whole purpose, and it makes it
     * useless for measuring speed, because the NRO calls guest_run in 5M
     * chunks and this calls it with a limit of 1. Bench mode reproduces the
     * NRO's call shape with no tracing, so interpreter changes can be measured
     * here instead of costing a hardware round trip. Correctness still comes
     * from the single-step differential; this only answers "how fast". */
    if (bench) {
        clock_t t0 = clock();
        double sec;
        st = GUEST_STEP_LIMIT;
        while (g.executed < limit && st == GUEST_STEP_LIMIT) {
            uint64_t chunk = limit - g.executed;
            if (chunk > 5000000ull)
                chunk = 5000000ull;
            st = guest_run(&g, 0xFFFFFFFFu, chunk);
        }
        sec = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
        printf("\nbench: %llu instructions in %.2f s = %.3f M instr/sec\n",
               (unsigned long long)g.executed, sec,
               sec > 0.0 ? (double)g.executed / sec / 1e6 : 0.0);
    }

    for (steps = 0; !bench && steps < limit; steps++) {
        uint32_t rec[2];
        uint64_t rel;
        g_steps = steps;
        rec[0] = g.cpu.r[15];
        if (g_trace_pc && !g_tracing && rec[0] == g_trace_pc &&
            ++g_pc_hits == g_trace_nth) {
            g_tracing = 1;
            g_window_step = steps;
            printf("  [diff ] trace window opens at pc=%08x hit #%llu, "
                   "step %llu\n", (unsigned)g_trace_pc,
                   (unsigned long long)g_pc_hits, (unsigned long long)steps);
        }
        if (g_trace && g_tracing) {
            rec[1] = state_hash(&g.cpu);
            fwrite(rec, 4, 2, g_trace);
            if (g_trace_count && ++g_trace_written >= g_trace_count) {
                printf("\n  [diff ] window closed: %llu records, steps "
                       "%llu..%llu\n", (unsigned long long)g_trace_written,
                       (unsigned long long)g_window_step,
                       (unsigned long long)steps);
                break;
            }
        }
        rel = (g_trace_at || g_trace_pc)
                  ? (g_tracing ? steps - g_window_step : ~0ull) : steps;
        if (rel >= full_start && rel < full_start + full_count) {
            unsigned k;
            printf("[%llu] pc=%08x %-5s it=%02x cpsr=%08x\n",
                   (unsigned long long)steps, (unsigned)g.cpu.r[15],
                   guest_is_thumb(&g.cpu) ? "Thumb" : "ARM",
                   (unsigned)g.cpu.itstate, (unsigned)g.cpu.cpsr);
            for (k = 0; k < 15; k++)
                printf(" r%-2u=%08x%s", k, (unsigned)g.cpu.r[k],
                       (k & 3) == 3 ? "\n" : "");
            printf("\n");
            /* VFP too: the state hash does not cover s[]/fpscr, so a wrong
             * float is invisible until it moves into a core register. */
            for (k = 0; k < 32; k++)
                printf(" d%-2u=%08x%08x%s", k,
                       (unsigned)g.cpu.s[(2 * k + 1) & 63],
                       (unsigned)g.cpu.s[(2 * k) & 63],
                       (k & 1) ? "\n" : "");
            printf("fpscr=%08x\n", (unsigned)g.cpu.fpscr);
        }
        st = guest_run(&g, 0xFFFFFFFFu, 1);
        if (g_watch_f) {
            uint32_t v;
            if (guest_ld32(&g.mem, g_watch_addr, &v) && v != g_watch_last) {
                uint32_t w[4];
                w[0] = (uint32_t)steps;
                w[1] = (uint32_t)(steps >> 32);
                w[2] = rec[0];          /* the instruction that just ran */
                w[3] = v;
                fwrite(w, 4, 4, g_watch_f);
                g_watch_last = v;
                g_watch_n++;
            }
        }
        if (st != GUEST_STEP_LIMIT)
            break;
    }
    if (g_trace)
        fclose(g_trace);
    if (g_watch_f) {
        fclose(g_watch_f);
        printf("watch: %llu changes to %08x\n", (unsigned long long)g_watch_n,
               (unsigned)g_watch_addr);
    }

    printf("\nstopped: %s after %llu instructions (%d imports)\n",
           guest_status_str(st == GUEST_STEP_LIMIT ? GUEST_STEP_LIMIT : st),
           (unsigned long long)g.executed, g_calls);
    if (st == GUEST_FAULT_UNDEF)
        printf("undef %08x at %08x (RVA %06x)\n", (unsigned)g.undef_insn,
               (unsigned)g.undef_pc,
               (unsigned)(g.undef_pc - g_img.load_base));
    if (st == GUEST_FAULT_MEM)
        printf("unmapped access at %08x\n", (unsigned)g.fault_addr);
    dump_regs("final", &g.cpu);
    dump_stub_calls();
    printf("heap: %u bytes  hooks m/r/f: %d/%d/%d  memsets: %d\n",
           (unsigned)(g_brk - HEAP_BASE), g_hook_hits[0], g_hook_hits[1],
           g_hook_hits[2], g_memsets);
    return 0;
}
