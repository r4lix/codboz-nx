/* guest.h -- ARM32 guest CPU state, memory and HLE boundary.
 *
 * The interface the interpreter implements now and a JIT implements later; see
 * README.md. Nothing here assumes which one is behind it.
 */
#ifndef GUEST_H
#define GUEST_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>     /* the inline accessors below use memcpy */

/* ---------------------------------------------------------------- registers */

enum { GUEST_SP = 13, GUEST_LR = 14, GUEST_PC = 15 };

/* CPSR bits we actually care about. T selects Thumb; the condition flags drive
 * every predicated instruction, and getting their edge cases right is the main
 * correctness risk (see README "Risks"). */
#define CPSR_T (1u << 5)
#define CPSR_V (1u << 28)
#define CPSR_Q (1u << 27)
#define CPSR_C (1u << 29)
#define CPSR_Z (1u << 30)
#define CPSR_N (1u << 31)

/* GE[3:0]. The ARMv6 parallel-arithmetic ops set one bit per byte lane and SEL
 * reads them back; they are a side channel between two instructions and are not
 * condition codes. The CRT's SIMD strlen is built on exactly that pair. */
#define CPSR_GE_SHIFT 16
#define CPSR_GE_MASK  (0xFu << CPSR_GE_SHIFT)

typedef struct {
    uint32_t r[16];
    uint32_t cpsr;
    /* softfp ABI: arguments and returns travel in r0-r3, but arithmetic uses
     * VFP, so single-precision state is required even though no float ever
     * crosses a function boundary in s0-s15. */
    /* 64 words = s0-s31 aliased over d0-d15, PLUS d16-d31. VFPv3-D32's upper
     * bank has no single-precision view, but this image uses it heavily (3003
     * instructions name d16-d31), and a 32-word file silently folded those
     * onto d0-d15 -- d22 landed on d6, d23 on d7, d27 on d11. */
    uint32_t s[64];
    uint32_t fpscr;
    /* Thumb ITSTATE, as cond<7:4>:mask<3:0>. Non-zero mask means the next
     * instructions are predicated; it advances once per instruction whether
     * that instruction executes or is skipped. */
    uint32_t itstate;
} GuestCpu;

static inline int guest_is_thumb(const GuestCpu *c) { return (c->cpsr & CPSR_T) != 0; }

/* ------------------------------------------------------------------- memory */

/* The guest address space is sparse (image, stack, heap, framebuffer, stubs),
 * so map regions rather than reserving 4 GB. Lookup stays behind a function to
 * leave room for a fastmem path later without touching call sites. */
#define GUEST_MAX_REGIONS 8

typedef struct {
    uint32_t base;      /* guest address */
    uint32_t size;
    uint8_t *host;      /* host backing store */
    int      writable;
} GuestRegion;

#define GUEST_UNDO_MAX 256u

typedef struct {
    uint32_t addr;
    uint32_t old;               /* value before the store */
    uint32_t val;               /* value the JIT left, filled in at check time */
    uint32_t size;
} GuestUndo;

typedef struct {
    GuestRegion region[GUEST_MAX_REGIONS];
    int         count;
    /* Index of the region the last lookup resolved to. Purely advisory: the
     * bounds are re-checked on every access, so a stale value costs one slow
     * lookup and can never produce a wrong answer. */
    int         cache;
    /* The same, for instruction fetch, which needs its own.
     *
     * One shared entry looked sufficient -- refreshed by whoever used it last,
     * and code is executed far more often than any single data region is
     * touched. It is not: code and data live in different regions, so every
     * guest load or store retargeted the entry and the NEXT instruction fetch
     * missed. Loads and stores are roughly a third of the stream, so a third
     * of all fetches were taking an out-of-line call and a linear region scan,
     * on the one path every single guest instruction runs. Splitting them
     * costs four bytes: code stays resolved here while data churns above. */
    int         icache;
    /* The resolved data region, denormalised out of region[cache].
     *
     * The interpreter can afford to index region[cache] because it is already
     * holding that pointer; this keeps the three fields it actually needs in
     * fixed slots. Written wherever `cache` is, which is only ever the slow
     * paths in guest.c.
     *
     * JIT code used to read these too, and no longer does -- one resolved
     * region is the wrong shape for a consumer that cannot re-resolve and
     * carry on, because a miss ends the block. It uses the slot tables below
     * instead. These remain the interpreter's own cache. */
    uint32_t    cache_base;
    uint32_t    cache_size;
    uint8_t    *cache_host;
    /* Whether that region may be written. The data cache is shared between
     * reads and writes, so it can be pointing at the image -- which is not
     * writable -- when a store arrives. (JIT code no longer consults this: its
     * write table simply leaves unwritable regions with size 0.) */
    uint32_t    cache_writable;
    /* And again for stores, which need their own.
     *
     * One shared data entry thrashes exactly the way the shared
     * instruction/data entry did: a block that loads a constant from the
     * read-only image and then stores to the heap points the entry at the
     * image on the load, so the store finds a region it may not write. In the
     * interpreter that costs a slow lookup; in JIT code it ends the block, and
     * it was throwing away 38% of everything compiled blocks were built to do.
     *
     * A write entry is only ever set from guest_wptr_slow, which refuses
     * unwritable regions, so anything found here is writable by construction
     * -- JIT store code needs no permission test at all. */
    int         wcache;
    uint32_t    wcache_base;
    uint32_t    wcache_size;
    uint8_t    *wcache_host;
    GuestUndo  *undo_log;       /* == guest_undo_log; here so JIT code can
                                 * reach it with one load off x0 */
    /* Optional four-byte data watch used during bring-up. The interpreter
     * stamps current_pc before every instruction; stores record the first
     * transition from non-zero to zero without changing normal semantics. */
    uint32_t    watch_addr;
    uint32_t    watch_value;
    uint32_t    watch_last_pc;
    uint32_t    watch_last_addr;
    uint32_t    watch_last_value;
    uint32_t    watch_zero_pc;
    uint32_t    watch_zero_addr;
    uint32_t    current_pc;
    uint64_t    watch_changes;
    /* Store undo log, for JIT verification only.
     *
     * Verification re-runs a block through the interpreter from the same
     * starting registers. That is only a fair comparison if it also starts
     * from the same MEMORY: a block doing ldr/add/str on one address would
     * have the re-run read back what the JIT just wrote and compute a
     * different answer, reporting a divergence that is an artefact of the
     * method rather than a bug in the lowering. Read-modify-write of a field
     * is far too common a shape to have that firing constantly.
     *
     * So every store records where it wrote and what was there before, and
     * verification rolls them back before re-running. Off unless a JIT
     * self-check is armed, so the ordinary path pays one predictable branch. */
    int         undo_active;
    uint32_t    undo_n;
    uint32_t    undo_overflow;  /* writes past the log's capacity */
    /* Slot tables for JIT-generated code (see GuestSlot). Pointers rather than
     * arrays for the same reason the undo log is external: 8 KB of table in
     * the middle of this struct would push the fields the interpreter reads on
     * every access onto different cache lines. */
    struct GuestSlot *rslot;
    struct GuestSlot *wslot;
    /* Fastmem window: every guest region aliased at fast_base + guest_addr, so
     * a translation is one add instead of a chain of dependent loads through
     * the region table. NULL when the mapping could not be set up, in which
     * case everything below falls back to the region path unchanged.
     *
     * The cost is that an unmapped guest address faults the process instead of
     * returning NULL, so guest_ptr can no longer report a fault gracefully.
     * That is why it is opt-in. */
    uint8_t *fast_base;
    /* Size of the aliased window. Guest addresses at or above it are NOT in
     * the window and must go the slow way, or the NULL contract that every HLE
     * handler relies on is silently broken: they pass a register straight to
     * guest_ptr and test the result, and registers hold garbage all the time
     * (s3eAccelerometerGetInt was called with r1 = 0xffffefb1). Without this,
     * that becomes a wild pointer 4 GB above the window instead of a NULL, and
     * the process dies with no diagnostic. The stub page at 0xF0000000 is
     * outside every achievable window too. */
    uint32_t fast_lo;           /* lowest mapped guest address */
    uint32_t fast_span;         /* mapped extent above fast_lo */
} GuestMem;

/* Guest address >> GUEST_SLOT_SHIFT -> the region owning that slot.
 *
 * The single denormalised cache above resolves one region at a time, which is
 * right for the interpreter (it re-resolves on a miss and carries on) and
 * wrong for JIT code (a miss ends the block). Real code alternates between
 * stack, heap and image constantly, so on hardware 68% of all block entries
 * were bailing before they retired a single instruction -- entering generated
 * code, missing the cache on a leading load, and handing the whole block back.
 *
 * Every region is at least 1 MB and starts on a 16 MB boundary, so indexing by
 * the top byte of the address resolves all four of them with no comparison at
 * all. A slot is filled only when its region covers the slot's FIRST byte,
 * which is what makes `addr - base` safe to compute before the bounds test:
 * the subtraction can never wrap into a small offset for an address that sits
 * just below the region. Anything not covered keeps size 0, and since the test
 * is unsigned `off + bytes-1 < size`, a zero size rejects every address --
 * the block bails and the interpreter faults properly, exactly as before. */
typedef struct GuestSlot {
    uint32_t base;
    uint32_t size;              /* 0 = unmapped here; every access bails */
    uint8_t *host;
} GuestSlot;

#define GUEST_SLOT_SHIFT 24
#define GUEST_SLOT_COUNT 256u

extern GuestSlot guest_rslot[GUEST_SLOT_COUNT];
extern GuestSlot guest_wslot[GUEST_SLOT_COUNT];

/* Rebuild both tables from the region list. Called by guest_mem_add. */
void guest_slots_build(GuestMem *m);

/* The log itself lives outside GuestMem so the struct the interpreter touches
 * on every access stays small: it is read on the hot path, and 4 KB of undo
 * entries in the middle of it would push cache_base and the region array onto
 * different lines. */
extern GuestUndo guest_undo_log[GUEST_UNDO_MAX];

/* Record a store about to happen. Only called when undo_active. */
void guest_undo_record(GuestMem *m, uint32_t addr, uint32_t size);

int   guest_mem_add(GuestMem *m, uint32_t base, uint32_t size, uint8_t *host, int writable);

/* Full-scan fallbacks, which also refresh the cache. Call the inline wrappers
 * below instead; these are only reached on a cache miss. */
void *guest_ptr_slow(const GuestMem *m, uint32_t addr, uint32_t len);
void *guest_wptr_slow(GuestMem *m, uint32_t addr, uint32_t len);
void *guest_ifetch_slow(const GuestMem *m, uint32_t addr, uint32_t len);

/* Returns NULL when the address is unmapped or the span crosses a region end;
 * callers must treat NULL as a guest fault, never as "skip".
 *
 * Inline with a one-entry cache because this is the interpreter's hottest
 * path: every instruction fetch and every guest load or store went through an
 * out-of-line call into another translation unit, and with no LTO that is a
 * real call plus a scan from region 0 -- to reach, nearly every time, the same
 * region as the access before it. */
static inline void *guest_ptr(const GuestMem *m, uint32_t addr, uint32_t len) {
    if (m->fast_base && (uint32_t)(addr - m->fast_lo) < m->fast_span)
        return m->fast_base + addr;
    const GuestRegion *r = &m->region[m->cache];
    uint32_t off = addr - r->base;
    if (off < r->size && len <= r->size - off)
        return r->host + off;
    return guest_ptr_slow(m, addr, len);
}

static inline void *guest_wptr(GuestMem *m, uint32_t addr, uint32_t len) {
    GuestRegion *r = &m->region[m->wcache];
    uint32_t off = addr - r->base;
    /* Store watch. watch_addr is 0 unless something armed it, so this is one
     * predictable compare; when armed it records the last writer's PC, which
     * is the only way to find what corrupts a guest word. current_pc is
     * stamped by guest_run. */
    /* Any store touching the watched word, not just one aligned exactly on it:
     * the clobber has appeared as 009f009e, which looks like two halfword
     * writes, and a store to watch_addr+2 must not slip past. */
    if (m->watch_addr && addr - m->watch_addr < 4u) {
        m->watch_last_addr = addr;
        m->watch_last_pc = m->current_pc;
        m->watch_changes++;
    }
    if (m->undo_active)
        guest_undo_record(m, addr, len);
    if (m->fast_base && (uint32_t)(addr - m->fast_lo) < m->fast_span)
        return m->fast_base + addr;
    if (r->writable && off < r->size && len <= r->size - off)
        return r->host + off;
    return guest_wptr_slow(m, addr, len);
}

/* Instruction fetch. Identical to guest_ptr but keyed on the fetch-only cache
 * entry; see the comment on GuestMem::icache for why that separation matters. */
static inline const void *guest_ifetch_ptr(const GuestMem *m, uint32_t addr,
                                           uint32_t len) {
    if (m->fast_base && (uint32_t)(addr - m->fast_lo) < m->fast_span)
        return m->fast_base + addr;
    const GuestRegion *r = &m->region[m->icache];
    uint32_t off = addr - r->base;
    if (off < r->size && len <= r->size - off)
        return r->host + off;
    return guest_ifetch_slow(m, addr, len);
}

#define GUEST_DEF_FETCH(bits, type)                                                static inline int guest_ifetch##bits(const GuestMem *m, uint32_t addr,                                              uint32_t *out) {                              const void *p = guest_ifetch_ptr(m, addr, (uint32_t)sizeof(type));             type v;                                                                        if (!p)                                                                            return 0;                                                                  memcpy(&v, p, sizeof v);                                                       *out = (uint32_t)v;                                                            return 1;                                                                  }
GUEST_DEF_FETCH(32, uint32_t)
GUEST_DEF_FETCH(16, uint16_t)
#undef GUEST_DEF_FETCH

/* All accessors report success rather than returning a value, so an unmapped
 * guest address can never be mistaken for a legitimate zero. memcpy rather
 * than a pointer cast: the image is mapped from an odd file offset, so every
 * access is potentially unaligned. */
#define GUEST_DEF_LOAD(bits, type)                                             \
    static inline int guest_ld##bits(const GuestMem *m, uint32_t addr,         \
                                     uint32_t *out) {                          \
        const void *p = guest_ptr(m, addr, (uint32_t)sizeof(type));            \
        type v;                                                                \
        if (!p)                                                                \
            return 0;                                                          \
        memcpy(&v, p, sizeof v);                                               \
        *out = (uint32_t)v;                                                    \
        return 1;                                                              \
    }
GUEST_DEF_LOAD(32, uint32_t)
GUEST_DEF_LOAD(16, uint16_t)
GUEST_DEF_LOAD(8,  uint8_t)
#undef GUEST_DEF_LOAD

#define GUEST_DEF_STORE(bits, type)                                            \
    static inline int guest_st##bits(GuestMem *m, uint32_t addr, uint32_t v) { \
        void *p = guest_wptr(m, addr, (uint32_t)sizeof(type));                 \
        type t = (type)v;                                                      \
        if (!p)                                                                \
            return 0;                                                          \
        memcpy(p, &t, sizeof t);                                               \
        return 1;                                                              \
    }
GUEST_DEF_STORE(32, uint32_t)
GUEST_DEF_STORE(16, uint16_t)
GUEST_DEF_STORE(8,  uint8_t)
#undef GUEST_DEF_STORE

/* --------------------------------------------------------------------- HLE */

/* Every import is reached by branching into a stub page: slot i lives at
 * GUEST_STUB_BASE + 4*i, matching the GOT bindings s3e_load() hands back. The
 * CPU checks for this range on branch, dispatches, and returns to LR -- no
 * guest instruction at those addresses is ever executed. */
#define GUEST_IPROF_N 768u

#define GUEST_STUB_BASE 0xF0000000u
#define GUEST_STUB_SIZE 0x1000u

typedef void (*GuestHleFn)(GuestCpu *cpu, GuestMem *mem, void *user);

typedef struct {
    const char *name;       /* import name, for tracing and diagnostics */
    GuestHleFn  fn;         /* NULL => unimplemented; log once and return 0 */
    void       *user;       /* per-slot cookie; the shared one is in GuestHle */
} GuestHleSlot;

typedef struct {
    GuestHleSlot *slot;
    uint32_t      count;
    void         *user;
} GuestHle;

/* Intercept a guest function by address: run the handler, then return to LR
 * without executing the original body. Needed because the app builds its own
 * allocator whose vtable is never populated in a stripped bring-up, and a port
 * replaces the guest allocator anyway. */
typedef struct {
    uint32_t   addr;    /* guest address; bit 0 ignored */
    GuestHleFn fn;
    void      *user;
    int        observe; /* non-zero: run fn, then execute the instruction
                         * normally instead of returning to LR */
} GuestHook;

static inline int guest_is_stub(uint32_t pc) {
    return (pc & ~1u) - GUEST_STUB_BASE < GUEST_STUB_SIZE;
}

static inline uint32_t guest_stub_index(uint32_t pc) {
    return ((pc & ~1u) - GUEST_STUB_BASE) / 4;
}

/* ------------------------------------------------------------------ the CPU */

typedef enum {
    GUEST_OK = 0,
    GUEST_HALTED,        /* returned to the landing pad */
    GUEST_FAULT_MEM,     /* unmapped access */
    GUEST_FAULT_UNDEF,   /* unimplemented encoding -- expected during bring-up */
    GUEST_STEP_LIMIT
} GuestStatus;

typedef struct {
    GuestCpu cpu;
    GuestMem mem;
    GuestHle hle;
    GuestHook *hook;
    uint32_t   hook_count;
    uint64_t executed;
    /* Ring of recently executed PCs. A branch through a null or stale pointer
     * lands somewhere with no context; this shows where it came from. */
    uint32_t hist[16];
    uint32_t hist_pos;
    uint32_t fault_addr;    /* set on FAULT_MEM */
    uint32_t undef_pc;      /* set on FAULT_UNDEF, with the raw encoding below */
    uint32_t undef_insn;
    /* Optional HLE profiler. Called around every import handler with the slot
     * index, enter=1 then enter=0. NULL disables it, so the cost is one
     * predictable branch on a path that runs thousands of times a frame, not
     * millions. Frame time splits into "interpreting guest code" and "inside a
     * handler", and only this boundary can tell them apart. */
    void   (*prof)(uint32_t slot, int enter);
    /* Optional guest-PC histogram: one counter per 16-byte bucket of the
     * loaded image, so a hot function shows up as a run of adjacent buckets.
     * 16 and not 64 because at 64 the first report put three separate library
     * functions in one span and there was no way to tell which of them was
     * hot -- resolution finer than a function is the whole point.
     * The HLE profiler above can only say "99% is not in a handler"; this says
     * which guest code that is, which is what decides whether the answer is a
     * native replacement for one routine or a faster interpreter for all of
     * them. Counts instructions rather than sampling time, which is the right
     * metric for "what would a native handler remove". NULL disables it. */
    uint32_t *pcprof;
    uint32_t  pcprof_base;      /* load base; bucket = (pc - base) >> 4 */
    uint32_t  pcprof_buckets;
    /* Optional instruction-class histogram, GUEST_IPROF_N counters:
     *   [0..255]    16-bit Thumb, indexed by hw >> 8
     *   [256..511]  32-bit Thumb, indexed by first halfword >> 8
     *   [512..767]  ARM, indexed by (insn >> 20) & 0xFF
     * pcprof says which code is hot; this says what that code is made of,
     * which is the question a decode fast path has to answer before it is
     * worth writing. NULL disables it. */
    uint32_t *iprof;
    void    *jit;           /* private hybrid-JIT context; NULL when disabled */
    uint64_t jit_executed;  /* guest instructions retired by compiled blocks */
    uint32_t jit_blocks;    /* successfully compiled basic blocks */
    /* Self-verification. The differential oracle in loader/run_boz.py cannot
     * test a JIT: it single-steps this interpreter against Unicorn, while a
     * JIT executes whole blocks, so the one method that has kept the CPU
     * honest so far simply does not apply. And the JIT only exists on AArch64,
     * so the host harness cannot run it either.
     *
     * So the device becomes the oracle. With this set, every block executed by
     * the JIT is re-run through the interpreter from the same starting state
     * and the resulting register files are compared. Slow -- it does the work
     * twice -- but it turns an ordinary play session into a correctness test,
     * and a JIT bug that survives to a save file three hours in is much worse
     * than a slow build.
     *
     * Safe only while compiled blocks contain no stores: re-running a block
     * that writes memory would apply its writes twice, and a block that reads
     * back what it wrote would diverge for that reason rather than a real bug.
     * Tier one lowers no memory operations at all, so the condition holds by
     * construction today; adding store lowering means adding an undo log
     * before this stays valid. jit_verify_blocks counts what has been checked,
     * so "no divergences" can be distinguished from "nothing was tested". */
    /* Block executions that ended early on a region-check miss, and the
     * instructions they gave up. A block compiled to eight instructions that
     * bails after two contributes two, so if bails are common the average
     * block length says nothing about how much was compiled -- which is
     * exactly the ambiguity that made four rounds of lowering look identical.
     * jit_bail_lost is what a better memory fast path would recover. */
    uint64_t jit_bails;
    uint64_t jit_bails_empty;   /* bailed before retiring anything */
    uint64_t jit_bail_lost;
    int      jit_verify;
    int      jit_chain;     /* link compiled blocks directly to each other */
    uint32_t jit_links;     /* chain edges installed */
    uint64_t jit_verify_blocks;
    uint32_t jit_diverged;
} Guest;

/* Hybrid A32/T32 -> AArch64 block cache. On non-AArch64 hosts these are
 * harmless stubs so the one-instruction differential harness remains the
 * reference path. `guest_jit_try_run` returns non-zero only when it executed a
 * compiled block and fills `retired` with that block's guest instruction
 * count. */
int  guest_jit_init(Guest *g);
/* Print what is terminating compiled blocks, weighted by executions. A no-op
 * when the JIT is off or on a host where it does not exist. */
void guest_jit_report_blockers(Guest *g);
void guest_jit_close(Guest *g);
int  guest_jit_try_run(Guest *g, uint32_t until, uint64_t remaining,
                       uint32_t *retired);

/* Run until `limit` instructions retire, PC reaches `until`, or a fault. */
GuestStatus guest_run(Guest *g, uint32_t until, uint64_t limit);

/* Call a guest function and come back -- the mechanism Marmalade callbacks
 * need. Saves and restores the interrupted register state. */
GuestStatus guest_call(Guest *g, uint32_t fn, uint32_t r0, uint32_t r1);

const char *guest_status_str(GuestStatus s);

#endif /* GUEST_H */
