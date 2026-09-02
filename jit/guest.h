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
} GuestMem;

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
    const GuestRegion *r = &m->region[m->cache];
    uint32_t off = addr - r->base;
    if (off < r->size && len <= r->size - off)
        return r->host + off;
    return guest_ptr_slow(m, addr, len);
}

static inline void *guest_wptr(GuestMem *m, uint32_t addr, uint32_t len) {
    GuestRegion *r = &m->region[m->cache];
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
    if (r->writable && off < r->size && len <= r->size - off)
        return r->host + off;
    return guest_wptr_slow(m, addr, len);
}

/* Instruction fetch. Identical to guest_ptr but keyed on the fetch-only cache
 * entry; see the comment on GuestMem::icache for why that separation matters. */
static inline const void *guest_ifetch_ptr(const GuestMem *m, uint32_t addr,
                                           uint32_t len) {
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
} Guest;

/* Hybrid A32/T32 -> AArch64 block cache. On non-AArch64 hosts these are
 * harmless stubs so the one-instruction differential harness remains the
 * reference path. `guest_jit_try_run` returns non-zero only when it executed a
 * compiled block and fills `retired` with that block's guest instruction
 * count. */
int  guest_jit_init(Guest *g);
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
