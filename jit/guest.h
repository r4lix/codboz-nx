/* guest.h -- ARM32 guest CPU state, memory and HLE boundary.
 *
 * The interface the interpreter implements now and a JIT implements later; see
 * README.md. Nothing here assumes which one is behind it.
 */
#ifndef GUEST_H
#define GUEST_H

#include <stddef.h>
#include <stdint.h>

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
/* Returns NULL when the address is unmapped or the span crosses a region end;
 * callers must treat NULL as a guest fault, never as "skip". */
void *guest_ptr(const GuestMem *m, uint32_t addr, uint32_t len);

/* All accessors report success rather than returning a value, so an unmapped
 * guest address can never be mistaken for a legitimate zero. */
int guest_ld32(const GuestMem *m, uint32_t addr, uint32_t *out);
int guest_ld16(const GuestMem *m, uint32_t addr, uint32_t *out);
int guest_ld8 (const GuestMem *m, uint32_t addr, uint32_t *out);
int guest_st32(GuestMem *m, uint32_t addr, uint32_t v);
int guest_st16(GuestMem *m, uint32_t addr, uint32_t v);
int guest_st8 (GuestMem *m, uint32_t addr, uint32_t v);

/* --------------------------------------------------------------------- HLE */

/* Every import is reached by branching into a stub page: slot i lives at
 * GUEST_STUB_BASE + 4*i, matching the GOT bindings s3e_load() hands back. The
 * CPU checks for this range on branch, dispatches, and returns to LR -- no
 * guest instruction at those addresses is ever executed. */
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
