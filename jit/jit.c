/* jit.c -- incremental ARMv7-A -> AArch64 basic-block JIT.
 *
 * This deliberately starts small. A block is compiled only when every
 * instruction up to its exit has an exact lowering; anything else falls back
 * to interp.c at the same PC. That makes coverage an optimisation property,
 * never a correctness property. The first tier handles the ordinary integer
 * arithmetic surrounding loads/stores and calls. Memory and VFP lowerings can
 * be added independently without changing the dispatcher contract.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"

#ifndef __SWITCH__

int guest_jit_init(Guest *g) { (void)g; return 0; }
void guest_jit_close(Guest *g) { (void)g; }
int guest_jit_try_run(Guest *g, uint32_t until, uint64_t remaining,
                      uint32_t *retired) {
    (void)g; (void)until; (void)remaining;
    *retired = 0;
    return 0;
}
void guest_jit_report_blockers(Guest *g) { (void)g; }

#else

#include <switch.h>

extern volatile uint32_t g_native_stage;

#define JIT_CODE_SIZE   (8u << 20)
/* 65536, not 32768. With the hotness filter below, occupancy is the hot
 * working set rather than every PC ever executed -- measured at 28827 entries
 * over a five-billion-instruction host run. That is 88% of a 32768-slot table,
 * and a 16-probe walk starts failing well before a table is actually full, so
 * the count is doubled to leave the probe chains short. */
#define JIT_CACHE_SLOTS 65536u
#define JIT_HOT_COUNT   3u
/* Tagged, direct-mapped hotness counters. 16384 entries x 4 bytes = 64 KB. */
#define JIT_HOT_SLOTS   16384u
#define JIT_MAX_GUEST   48u
/* 1536, not 768. The slot-table resolve is about sixteen words where the
 * single-region check was eight, so a block of 48 guest instructions with a
 * memory access in most of them no longer fits. The loop guards below stop
 * cleanly when the buffer fills, but a block cut short for want of space is
 * one the JIT stops early in for no architectural reason. 6 KB of stack in
 * compile_block is cheaper than losing block length. */
#define JIT_MAX_A64     1536u

typedef uint32_t (*JitBlockFn)(Guest *g);
typedef uint32_t (*JitEnterFn)(Guest *g, void *blk, uint32_t budget);

/* Exactly 12 bytes, and that is the whole point.
 *
 * This was 36 bytes across 65536 slots -- 2.25 MB, indexed by a multiplicative
 * hash, against a 2 MB L2 shared with the rest of the system. The hash was the
 * worst part: PCs are dense and local, and hashing them deliberately destroys
 * the locality that would have kept the live part of the table resident. Every
 * block dispatch was a probable L2 miss before it even reached the code cache.
 *
 * That is the shape the measurements were describing all along. Raising
 * coverage from 1% to 67% changed the frame rate not at all, and removing 970M
 * bailed instructions changed it not at all; both are only possible if the
 * cost is per-BLOCK rather than per-instruction, and at ~5.5 guest
 * instructions per block a dispatch was costing more than interpreting the
 * block it dispatched to.
 *
 * So the fields a dispatch actually touches live here, and everything that
 * exists for reporting or for installing a link -- which happens once per edge,
 * not once per execution -- moved to JitCold alongside. */
typedef struct {
    uint32_t key;               /* PC | Thumb bit */
    uint32_t rx_off;
    uint16_t count;
    uint8_t  state;             /* 0 empty, 1 claimed, 2 compiled, 3 reject */
    uint8_t  linked;            /* 1 also means "has no link slot to install" */
} JitEntry;

/* Same index as JitEntry, touched only when compiling, installing a link, or
 * reporting. Kept out of the dispatch path entirely. */
typedef struct {
    uint32_t execs;
    uint32_t end_insn;          /* 0 when the block ended on its own terms */
    uint32_t link_word;         /* 0 = block has no link slot */
    uint32_t link_key;
    uint8_t  end_thumb;
} JitCold;

/* One hotness counter. No PC is stored -- a 16-bit tag taken from bits of the
 * hash that the index does not use is enough to tell two PCs apart cheaply,
 * and a tag collision costs at worst one wasted block-table slot. */
typedef struct {
    uint16_t tag;
    uint8_t  hits;
    uint8_t  pad;
} JitCount;

typedef struct {
    Jit code;
    uint8_t *rw, *rx;
    uint32_t used;
    JitEntry *entry;
    JitCold  *cold;
    JitCount *count;
    uint32_t enter_off;         /* trampoline that owns w19/w20 */
    JitEntry *last;             /* block executed on the previous dispatch */
    int chain;                  /* linking enabled for this run */
    uint32_t links;             /* chain edges installed */
    int ready;
    int probe_only;             /* stage-one hardware validation gate */
} JitContext;

typedef struct {
    uint32_t code[JIT_MAX_A64];
    uint32_t n;
    uint32_t link_word;         /* index of the chain branch, 0 if none */
    uint32_t link_key;          /* successor PC|thumb */
} A64;

static void emit(A64 *a, uint32_t insn) {
    if (a->n < JIT_MAX_A64)
        a->code[a->n++] = insn;
}

/* A64 encoders. The JIT keeps architectural registers in GuestCpu memory for
 * now, using w9-w12 as temporaries. This leaves x0 (Guest*) live throughout a
 * leaf block and requires no prologue or ABI spills. */
static void e_ldr_w(A64 *a, unsigned rt, unsigned byte_off) {
    emit(a, 0xB9400000u | ((byte_off >> 2) << 10) | (0u << 5) | rt);
}
/* LDR Wt,[Xn,#off] and LDR Xt,[Xn,#off] -- the existing pair is hardwired to
 * x0, and the slot table is reached through a pointer in another register. */
static void e_ldr_off(A64 *a, unsigned rt, unsigned rn, unsigned byte_off) {
    emit(a, 0xB9400000u | ((byte_off >> 2) << 10) | (rn << 5) | rt);
}
static void e_ldr64_off(A64 *a, unsigned rt, unsigned rn, unsigned byte_off) {
    emit(a, 0xF9400000u | ((byte_off >> 3) << 10) | (rn << 5) | rt);
}
static void e_str_w(A64 *a, unsigned rt, unsigned byte_off) {
    emit(a, 0xB9000000u | ((byte_off >> 2) << 10) | (0u << 5) | rt);
}
static void e_mov32(A64 *a, unsigned rd, uint32_t v) {
    emit(a, 0x52800000u | ((v & 0xFFFFu) << 5) | rd);       /* MOVZ Wd */
    if (v >> 16)
        emit(a, 0x72A00000u | (((v >> 16) & 0xFFFFu) << 5) | rd); /* MOVK LSL16 */
}
static void e_rr(A64 *a, uint32_t base, unsigned rd, unsigned rn, unsigned rm) {
    emit(a, base | (rm << 16) | (rn << 5) | rd);
}
static void e_load_r(A64 *a, unsigned wt, unsigned guest_r) {
    e_ldr_w(a, wt, (unsigned)offsetof(Guest, cpu.r[guest_r]));
}
static void e_store_r(A64 *a, unsigned wt, unsigned guest_r) {
    e_str_w(a, wt, (unsigned)offsetof(Guest, cpu.r[guest_r]));
}
static void e_set_pc(A64 *a, uint32_t pc) {
    e_mov32(a, 9, pc);
    e_store_r(a, 9, GUEST_PC);
}

/* Copy host NZCV (same bit positions as CPSR) into the selected guest flag
 * bits. ADD/SUB use all four; logical instructions update N/Z and preserve
 * guest C/V exactly as ARMv7 requires for an unshifted operand. */
static void e_merge_nzcv(A64 *a, uint32_t mask) {
    emit(a, 0xD53B420Cu);                         /* MRS x12, NZCV */
    e_ldr_w(a, 10, (unsigned)offsetof(Guest, cpu.cpsr));
    e_mov32(a, 11, mask);
    e_rr(a, 0x0A200000u, 10, 10, 11);             /* BIC w10,w10,w11 */
    e_rr(a, 0x0A000000u, 12, 12, 11);             /* AND w12,w12,w11 */
    e_rr(a, 0x2A000000u, 10, 10, 12);             /* ORR w10,w10,w12 */
    e_str_w(a, 10, (unsigned)offsetof(Guest, cpu.cpsr));
}

static void e_set_nz(A64 *a, unsigned wr) {
    e_rr(a, 0x6A000000u, 31, wr, wr);              /* ANDS wzr,wr,wr */
    e_merge_nzcv(a, CPSR_N | CPSR_Z);
}

static void e_return(A64 *a, uint32_t retired) {
    emit(a, 0x11000000u | ((retired & 0xFFFu) << 10) | (19u << 5) | 19u);
                                                   /* ADD w19,w19,#retired */
    emit(a, 0xD65F03C0u);                          /* RET */
}

/* Leave the block at a PC known while compiling, with a slot to chain from.
 *
 * The slot starts as a branch to the RET immediately after it, so an unlinked
 * block behaves exactly as e_return did. Patching that one word to point at
 * the successor's entry turns the return, the C-side hash lookup and the
 * re-entry into a single branch -- which is the entire point: at three guest
 * instructions per block, the dispatcher costs more than the block does.
 *
 * w20 carries the remaining instruction budget so a chain cannot outrun the
 * limit guest_run was called with; when it goes non-positive the chain stops
 * and control returns to C, which re-checks hooks, the halt address and
 * everything else it is responsible for. */
static void e_return_linkable(A64 *a, uint32_t retired, uint32_t next_key) {
    emit(a, 0x11000000u | ((retired & 0xFFFu) << 10) | (19u << 5) | 19u);
                                                   /* ADD  w19,w19,#retired */
    emit(a, 0x71000000u | ((retired & 0xFFFu) << 10) | (20u << 5) | 20u);
                                                   /* SUBS w20,w20,#retired */
    emit(a, 0x54000000u | (2u << 5) | 13u);        /* B.LE +2 -> the RET */
    a->link_word = a->n;
    a->link_key = next_key;
    emit(a, 0x14000001u);                          /* B +1 -> the RET */
    emit(a, 0xD65F03C0u);                          /* RET */
}

/* Leave the block at `pc`, having retired `n` instructions. Exactly five
 * instructions, always, so the branch that skips it can be a fixed distance
 * and nothing needs patching afterwards. MOVK is emitted even when the top
 * halfword is zero for that reason -- guest PCs are 0x4axxxxxx so it is never
 * actually redundant, but the size must not depend on the value. */
static void e_bail(A64 *a, uint32_t pc, uint32_t n) {
    emit(a, 0x52800000u | ((pc & 0xFFFFu) << 5) | 9u);          /* MOVZ w9 */
    emit(a, 0x72A00000u | (((pc >> 16) & 0xFFFFu) << 5) | 9u);  /* MOVK lsl16 */
    e_store_r(a, 9, GUEST_PC);
    /* Still exactly five instructions: the count moved from w0 into the w19
     * accumulator, which is what makes a chain of blocks add up. Keeping the
     * size identical matters -- several branch-over-the-bail distances are
     * hardcoded as +6 and would all have to change together. */
    emit(a, 0x11000000u | ((n & 0xFFFu) << 10) | (19u << 5) | 19u); /* ADD w19,w19,#n */
    emit(a, 0xD65F03C0u);                                       /* RET */
}

/* Resolve a guest address through the slot table.
 *
 * In:  w9 = guest address.  Out: w9 = offset into the region, x12 = host base,
 * so the caller's access is still one `LDR/STR wN,[x12,w9,uxtw]`. Clobbers
 * w10 and w11. On a miss the block bails at `pc` having retired `n`.
 *
 * This replaces a bounds test against the ONE region the interpreter last
 * resolved. That test was cheap and wrong-shaped: real code alternates between
 * stack, heap and image, so the cached region was usually not the one being
 * accessed, and the block ended. Measured on hardware, 68% of block entries
 * bailed without retiring a single instruction and 970M instructions were lost
 * to bails against 2552M retired. Indexing a table by the top byte of the
 * address resolves any region in the same handful of instructions and makes a
 * bail mean what it should -- genuinely unmapped, or an access running off a
 * region end -- rather than "wrong region cached".
 *
 * `span` is the number of bytes touched; the test is unsigned
 * `off + span-1 < size`, which a zero size (an unmapped slot) always fails. */
static int g_fastmem;           /* set at init from GuestMem.fast_base */

static void e_resolve(A64 *a, int write, uint32_t span, uint32_t pc,
                      uint32_t n) {
    unsigned mem = (unsigned)offsetof(Guest, mem);
    /* One instruction instead of sixteen.
     *
     * With every region aliased at fast_base + guest_addr there is nothing to
     * resolve and nothing to bounds-check: the guest address is 32 bits and
     * the window is 4 GB, so w9 is already the offset the access wants and
     * x12 is already the base it wants. The caller's
     * `LDR/STR wN,[x12,w9,uxtw]` is unchanged, which is why this drops in
     * without touching a single call site.
     *
     * This is also most of the code cache: at ~35% memory operations across
     * ~168k compiled guest instructions, the fifteen words this removes are
     * around 3 MB of emitted code -- and the dispatch working set is what has
     * actually been costing frames. */
    if (g_fastmem) {
        unsigned foff = mem + (unsigned)offsetof(GuestMem, fast_base);
        unsigned loff = mem + (unsigned)offsetof(GuestMem, fast_lo);
        unsigned poff = mem + (unsigned)offsetof(GuestMem, fast_span);
        /* Same single-compare trick the interpreter uses: subtract the lowest
         * mapped address, and one unsigned test then rejects both the
         * near-null pointers below the regions and anything above them.
         * Eleven words against the region path's sixteen, and none of it is a
         * dependent chain -- the two loads are independent of each other. */
        e_ldr_w(a, 10, loff);                    /* w10 = fast_lo */
        e_rr(a, 0x4B000000u, 9, 9, 10);          /* w9 = addr - fast_lo */
        e_ldr_w(a, 10, poff);                    /* w10 = fast_span */
        e_rr(a, 0x6B000000u, 31, 9, 10);         /* CMP w9,w10 */
        emit(a, 0x54000000u | (6u << 5) | 3u);   /* B.LO +6 (in span) */
        e_bail(a, pc, n);
        emit(a, 0xF9400000u | ((foff >> 3) << 10) | (0u << 5) | 12u);
        /* x12 must be the base biased by fast_lo, since w9 is now an offset
         * from there rather than a raw guest address. */
        e_ldr_w(a, 11, loff);
        emit(a, 0x8B2B4000u | (11u << 16) | (12u << 5) | 12u); /* ADD x12,x12,w11,uxtw */
        (void)write; (void)span;
        return;
    }
    unsigned toff = mem + (unsigned)(write ? offsetof(GuestMem, wslot)
                                           : offsetof(GuestMem, rslot));
    emit(a, 0xF9400000u | ((toff >> 3) << 10) | (0u << 5) | 12u);
                                                 /* LDR x12,[x0,#slot table] */
    emit(a, 0x53000000u | (GUEST_SLOT_SHIFT << 16) | (31u << 10) |
            (9u << 5) | 11u);                    /* LSR w11,w9,#24 */
    emit(a, 0x8B200000u | (11u << 16) | (2u << 13) | (4u << 10) |
            (12u << 5) | 11u);                   /* ADD x11,x12,w11,UXTW #4 */
    e_ldr_off(a, 10, 11, 0);                     /* w10 = slot->base */
    e_rr(a, 0x4B000000u, 9, 9, 10);              /* w9 = addr - base */
    e_ldr_off(a, 10, 11, 4);                     /* w10 = slot->size */
    if (span > 1u) {
        emit(a, 0x11000000u | ((span - 1u) << 10) | (9u << 5) | 12u);
                                                 /* ADD w12,w9,#span-1 */
        e_rr(a, 0x6B000000u, 31, 12, 10);        /* CMP w12,w10 */
    } else {
        e_rr(a, 0x6B000000u, 31, 9, 10);         /* CMP w9,w10 */
    }
    emit(a, 0x54000000u | (6u << 5) | 3u);       /* B.LO +6 (skip the bail) */
    e_bail(a, pc, n);
    e_ldr64_off(a, 12, 11, 8);                   /* x12 = slot->host */
}

/* Guest load, address in w9, result into w9.
 *
 * The bounds test is against the region the interpreter last resolved, and a
 * miss simply ends the block at this instruction. That is what keeps this
 * lowering honest without modelling anything difficult: an unmapped address,
 * an access crossing a region end, and an access to a region other than the
 * cached one all take the same exit, and the interpreter then handles the
 * instruction exactly as it would have anyway -- faulting properly if it must,
 * and refreshing the cache if it can, so the next pass through the block
 * succeeds. No fault path, no helper call, no frame; the block stays a leaf.
 *
 * `bytes` is 4 or 1. Halfword and signed forms are deliberately absent rather
 * than guessed at; they can be added the same way once these are proven. */
static void e_load_mem(A64 *a, int bytes, uint32_t pc, uint32_t n) {
    e_resolve(a, 0, (uint32_t)bytes, pc, n);     /* w9 = off, x12 = host */
    if (bytes == 4)
        emit(a, 0xB8604800u | (9u << 16) | (12u << 5) | 9u);  /* LDR  w9,[x12,w9,uxtw] */
    else if (bytes == 2)
        emit(a, 0x78604800u | (9u << 16) | (12u << 5) | 9u);  /* LDRH w9,[x12,w9,uxtw] */
    else
        emit(a, 0x38604800u | (9u << 16) | (12u << 5) | 9u);  /* LDRB w9,[x12,w9,uxtw] */
}

/* ThumbExpandImm, evaluated at compile time -- imm12 is a constant in the
 * encoding, so the whole expansion folds away and only the value is emitted.
 * Mirrors thumb_expand_imm() in interp.c exactly; the carry-out is returned so
 * the caller can decline the cases where it would have to be modelled. */
static uint32_t jit_thumb_imm(uint32_t imm12, int *rotated) {
    uint32_t b = imm12 & 0xFFu;
    *rotated = 0;
    if ((imm12 & 0xC00u) == 0) {
        switch ((imm12 >> 8) & 3u) {
        case 0:  return b;
        case 1:  return (b << 16) | b;
        case 2:  return (b << 24) | (b << 8);
        default: return (b << 24) | (b << 16) | (b << 8) | b;
        }
    }
    {
        uint32_t unrot = 0x80u | (imm12 & 0x7Fu);
        int rot = (int)((imm12 >> 7) & 0x1Fu);
        *rotated = 1;                    /* carry-out comes from the result */
        return (unrot >> rot) | (unrot << (32 - rot));
    }
}

/* T32 op field -> the ARM-style opcode emit_dp() speaks. Returns -1 for the
 * forms not lowered: ORN, RSB, ADC and SBC, which need the carry-in modelled
 * or an operand inversion emit_dp has no encoding for. */
static int t32_dp_op(uint32_t op) {
    switch (op) {
    case 0x0: return 0x0;    /* AND */
    case 0x1: return 0xE;    /* BIC */
    case 0x2: return 0xC;    /* ORR  (MOV when Rn == 15) */
    case 0x4: return 0x1;    /* EOR */
    case 0x8: return 0x4;    /* ADD */
    case 0xD: return 0x2;    /* SUB  (CMP when Rd == 15 and S) */
    default:  return -1;
    }
}

/* Interworking branch to the address in w9: PC = value & ~1, CPSR.T = value&1.
 *
 * This is what a function return looks like in both instruction sets -- ARM
 * BX, Thumb BX, and the register forms of BLX -- and it was the single largest
 * ARM block terminator. The T bit has to move with the branch or the very next
 * instruction is decoded in the wrong instruction set, so it is written even
 * though almost every branch here stays in Thumb.
 *
 * The masks go through a register rather than an AND-immediate: AArch64's
 * logical immediates are a bitmask-encoded field, and hand-deriving one is how
 * `and w9,w9,#0xfffffffe` silently becomes `and w9,w9,#2`. */
static void e_interwork(A64 *a) {
    e_ldr_w(a, 10, (unsigned)offsetof(Guest, cpu.cpsr));
    e_mov32(a, 11, ~CPSR_T);
    e_rr(a, 0x0A000000u, 10, 10, 11);            /* clear T */
    emit(a, 0x12000000u | (9u << 5) | 12u);      /* AND w12,w9,#1 */
    emit(a, 0x2A000000u | (12u << 16) | (5u << 10) | (10u << 5) | 10u);
                                                 /* ORR w10,w10,w12,LSL #5 */
    e_str_w(a, 10, (unsigned)offsetof(Guest, cpu.cpsr));
    e_mov32(a, 11, 0xFFFFFFFEu);
    e_rr(a, 0x0A000000u, 9, 9, 11);              /* AND w9,w9,w11 */
    e_store_r(a, 9, GUEST_PC);
}

/* Guest store: guest address in w9, value in w14.
 *
 * Same shape as e_load_mem -- bounds-check the cached region, bail to the
 * interpreter on a miss -- with two additions. The region must be writable,
 * because the data cache is shared with loads and can be pointing at the
 * image when a store arrives; without that test JIT code would write into
 * memory the interpreter refuses. And under verification each store records
 * what it is about to overwrite, so the re-run can start from the same memory.
 *
 * The undo record is emitted only when verifying, but the store sequence
 * either side of it is identical in both builds, so what gets checked is the
 * code that ships. */
static void e_store_mem(A64 *a, int bytes, uint32_t pc, uint32_t n, int verify) {
    unsigned mem = (unsigned)offsetof(Guest, mem);
    e_rr(a, 0x2A000000u, 15, 31, 9);             /* MOV w15,w9 (keep address) */
    /* No permission test: the write table only ever holds writable regions,
     * exactly as the write cache it replaces only ever did. */
    e_resolve(a, 1, (uint32_t)bytes, pc, n);     /* w9 = off, x12 = host */

    if (verify) {
        unsigned noff = mem + (unsigned)offsetof(GuestMem, undo_n);
        e_ldr_w(a, 10, noff);
        e_mov32(a, 11, GUEST_UNDO_MAX);
        e_rr(a, 0x6B000000u, 31, 10, 11);        /* CMP w10,w11 */
        emit(a, 0x54000000u | (6u << 5) | 3u);   /* B.LO +6 (room: skip bail) */
        e_bail(a, pc, n);
        /* old value, same width as the store */
        if (bytes == 4)      emit(a, 0xB8604800u | (9u << 16) | (12u << 5) | 13u);
        else if (bytes == 2) emit(a, 0x78604800u | (9u << 16) | (12u << 5) | 13u);
        else                 emit(a, 0x38604800u | (9u << 16) | (12u << 5) | 13u);
        emit(a, 0xF9400000u |
                (((mem + (unsigned)offsetof(GuestMem, undo_log)) >> 3) << 10) |
                (0u << 5) | 11u);                /* LDR x11,[x0,#undo_log] */
        emit(a, 0x8B000000u | (10u << 16) | (4u << 10) | (11u << 5) | 11u);
                                                 /* ADD x11,x11,x10,LSL #4 */
        emit(a, 0xB9000000u | (0u << 10) | (11u << 5) | 15u);   /* str w15,[x11] */
        emit(a, 0xB9000000u | (1u << 10) | (11u << 5) | 13u);   /* str w13,[x11,#4] */
        emit(a, 0xB9000000u | (2u << 10) | (11u << 5) | 31u);   /* str wzr,[x11,#8] */
        e_mov32(a, 13, (uint32_t)bytes);
        emit(a, 0xB9000000u | (3u << 10) | (11u << 5) | 13u);   /* str w13,[x11,#12] */
        emit(a, 0x11000400u | (10u << 5) | 10u);                /* ADD w10,w10,#1 */
        e_str_w(a, 10, noff);
    }

    if (bytes == 4)
        emit(a, 0xB8204800u | (9u << 16) | (12u << 5) | 14u);   /* STR  w14 */
    else if (bytes == 2)
        emit(a, 0x78204800u | (9u << 16) | (12u << 5) | 14u);   /* STRH w14 */
    else
        emit(a, 0x38204800u | (9u << 16) | (12u << 5) | 14u);   /* STRB w14 */
}

static uint32_t arm_rotimm(uint32_t insn) {
    uint32_t v = insn & 0xFFu, rot = ((insn >> 8) & 0xFu) * 2u;
    return rot ? ((v >> rot) | (v << (32u - rot))) : v;
}

static int has_hook(const Guest *g, uint32_t pc) {
    uint32_t i;
    for (i = 0; i < g->hook_count; i++)
        if ((g->hook[i].addr & ~1u) == pc)
            return 1;
    return 0;
}

/* Emit a data-processing result. w9=a/result, w10=b. */
static int emit_dp(A64 *a, uint32_t op, int setflags, int writes,
                   uint32_t rd) {
    uint32_t base;
    /* Whether the emitted instruction actually sets NZCV itself. It must, or
     * the merge below takes whatever the host flags happened to hold -- and
     * they hold something: the region bounds test in e_resolve ends in a CMP,
     * so a flag-setting logical op two instructions after a load was reading
     * the result of comparing an offset against a region size.
     *
     * That is what made BICS wrong. AArch64 has ANDS and BICS but no ORRS and
     * no EORS, so those two have to set the flags with a separate test, and
     * BIC simply had no S-form selected at all. All three merged stale flags.
     * Unreachable until the slot table stopped blocks bailing at their first
     * load, then immediately wrong on hardware. */
    int self_flags = 1;
    switch (op) {
    case 0x0: base = setflags ? 0x6A000000u : 0x0A000000u; break; /* AND/S */
    case 0x2: base = setflags ? 0x6B000000u : 0x4B000000u; break;/* SUB/S */
    case 0x4: base = setflags ? 0x2B000000u : 0x0B000000u; break;/* ADD/S */
    case 0xE: base = setflags ? 0x6A200000u : 0x0A200000u; break;/* BIC/S */
    case 0x1: base = 0x4A000000u; self_flags = 0; break;         /* EOR */
    case 0xC: base = 0x2A000000u; self_flags = 0; break;         /* ORR */
    default: return 0;
    }
    /* An op with no S-form has to keep its result somewhere testable, so it
     * computes into w9 even when the architectural result is discarded. */
    e_rr(a, base, (writes || (setflags && !self_flags)) ? 9u : 31u, 9, 10);
    if (setflags) {
        if (!self_flags)
            e_set_nz(a, 9);                      /* ANDS wzr,w9,w9 then merge */
        else if (op == 2 || op == 4)
            e_merge_nzcv(a, CPSR_N | CPSR_Z | CPSR_C | CPSR_V);
        else
            e_merge_nzcv(a, CPSR_N | CPSR_Z);
    }
    if (writes)
        e_store_r(a, 9, rd);
    return 1;
}

static int compile_arm(Guest *g, A64 *a, uint32_t start, uint32_t until,
                       uint16_t *out_count, uint32_t *out_end) {
    uint32_t pc = start, n = 0;
    *out_end = 0;
    while (n < JIT_MAX_GUEST && a->n + 48 < JIT_MAX_A64) {
        uint32_t insn, op, S, rn, rd;
        /* Reaching a hook, the halt address, or unreadable memory ends the
         * block for reasons that have nothing to do with an encoding. Clearing
         * out_end keeps them out of the blocker report -- left in, they show
         * up as whatever instruction happened to precede them, with counts no
         * lowering can ever move, which is precisely how ADD-immediate came to
         * look like it was ending 11% of blocks. */
        if (pc == (until & ~1u) || has_hook(g, pc) ||
            !guest_ld32(&g->mem, pc, &insn)) {
            *out_end = 0;
            break;
        }
        /* Assigned before the condition test, not with it. Folded together,
         * a conditional instruction ending the block left out_end holding the
         * PREVIOUS instruction -- which reported "CMP ends 14% of blocks", an
         * encoding the JIT lowers perfectly well, when the real blocker was
         * the predicated instruction after it. */
        *out_end = insn;
        if ((insn >> 28) != 0xEu)
            break;

        if ((insn & 0x0FBF0000u) == 0x028F0000u) {       /* ADD/SUB Rd,PC,#imm */
            /* ADR. PC is known while compiling, so it folds to a constant --
             * and the general path below refuses Rn == PC, which was ending
             * 11% of blocks for an instruction with no runtime behaviour. */
            uint32_t rdx = (insn >> 12) & 0xFu;
            uint32_t base = pc + 8u, imm = arm_rotimm(insn);
            if (rdx == GUEST_PC) break;
            e_mov32(a, 9, (insn & 0x00800000u) ? base + imm : base - imm);
            e_store_r(a, 9, rdx);
            pc += 4; n++;
            continue;
        }

        if ((insn & 0x0FFFFFF0u) == 0x012FFF10u) {       /* BX Rm */
            e_load_r(a, 9, insn & 0xFu);
            e_interwork(a);
            n++; e_return(a, n); *out_count = (uint16_t)n; *out_end = 0;
            return 1;
        }

        if ((insn & 0x0E000000u) == 0x0A000000u) {       /* B / BL */
            int32_t off = (int32_t)(insn << 8) >> 6;
            if (insn & 0x01000000u) {
                e_mov32(a, 9, pc + 4u);
                e_store_r(a, 9, GUEST_LR);
            }
            e_set_pc(a, pc + 8u + (uint32_t)off);
            n++;
            /* A taken branch to a constant address: the target is a real
             * instruction and a block will exist there, so this is the exit
             * that is actually worth chaining. */
            e_return_linkable(a, n, pc + 8u + (uint32_t)off);
            *out_count = (uint16_t)n;
            *out_end = 0;       /* ended on its own terms, not blocked */
            return 1;
        }

        if ((insn & 0x0FF00000u) == 0x03000000u) {       /* MOVW */
            rd = (insn >> 12) & 0xFu;
            if (rd == GUEST_PC) break;
            e_mov32(a, 9, ((insn >> 4) & 0xF000u) | (insn & 0xFFFu));
            e_store_r(a, 9, rd);
            pc += 4; n++;
            continue;
        }
        if ((insn & 0x0FF00000u) == 0x03400000u) {       /* MOVT */
            uint32_t imm = ((insn >> 4) & 0xF000u) | (insn & 0xFFFu);
            rd = (insn >> 12) & 0xFu;
            if (rd == GUEST_PC) break;
            e_load_r(a, 9, rd);
            emit(a, 0x72A00000u | (imm << 5) | 9u);      /* MOVK w9,#imm,lsl16 */
            e_store_r(a, 9, rd);
            pc += 4; n++;
            continue;
        }

        /* LDR / LDRB, immediate offset, no writeback. The single most common
         * shape, and the one that was ending every block: bits 26-27 select
         * the whole load/store space, so the test below used to reject it
         * outright and blocks averaged 1.5 instructions as a result.
         *
         * Restricted deliberately to P=1 W=0 (offset addressing, no base
         * update), I=0 (immediate), L=1 (load). Stores are left out until the
         * verification undo log exists, since re-running a block that writes
         * memory would apply its writes twice. Register-offset and writeback
         * forms are simply not lowered yet -- they break the block as before,
         * which costs coverage and nothing else. */
        if ((insn & 0x0C000000u) == 0x04000000u) {
            uint32_t P = (insn >> 24) & 1u, U = (insn >> 23) & 1u;
            uint32_t B = (insn >> 22) & 1u, W = (insn >> 21) & 1u;
            uint32_t L = (insn >> 20) & 1u, I = (insn >> 25) & 1u;
            uint32_t imm = insn & 0xFFFu;
            rn = (insn >> 16) & 0xFu;
            rd = (insn >> 12) & 0xFu;
            if (!L || I || !P || W || rn == GUEST_PC || rd == GUEST_PC)
                break;
            e_load_r(a, 9, rn);
            if (imm) {
                /* ADD/SUB immediate: 12 bits, exactly the ARM field width. */
                emit(a, (U ? 0x11000000u : 0x51000000u) |
                        (imm << 10) | (9u << 5) | 9u);
            }
            e_load_mem(a, B ? 1 : 4, pc, n);
            e_store_r(a, 9, rd);
            pc += 4; n++;
            continue;
        }

        if ((insn & 0x0C000000u) != 0 ||
            ((insn & 0x0E000000u) == 0 && (insn & 0x90u) == 0x90u))
            break;
        op = (insn >> 21) & 0xFu;
        S = (insn >> 20) & 1u;
        rn = (insn >> 16) & 0xFu;
        rd = (insn >> 12) & 0xFu;
        if ((!S && (op & 0xCu) == 0x8u) || rd == GUEST_PC ||
            (rn == GUEST_PC && op != 0xDu && op != 0xFu))
            break;

        /* Operand 2: modified immediate or an unshifted core register. */
        if (insn & 0x02000000u) {
            e_mov32(a, 10, arm_rotimm(insn));
        } else {
            uint32_t rm = insn & 0xFu;
            if ((insn & 0xFF0u) || rm == GUEST_PC)
                break;
            e_load_r(a, 10, rm);
        }

        if (S && (op == 0 || op == 1 || op == 0xC || op == 0xD ||
                  op == 0xE || op == 0xF))
            break;                                        /* shifter carry tier 2 */
        if (op == 0xDu || op == 0xFu) {                  /* MOV / MVN */
            if (S) break;                                /* shifter carry */
            if (op == 0xD)
                e_rr(a, 0x2A000000u, 9, 31, 10);        /* MOV = ORR wzr */
            else
                e_rr(a, 0x2A200000u, 9, 31, 10);        /* MVN = ORN wzr */
            e_store_r(a, 9, rd);
        } else {
            int writes = !(op >= 8u && op <= 0xBu);
            if (op == 0xAu) {                            /* CMP = SUBS */
                e_load_r(a, 9, rn);
                if (!emit_dp(a, 2, 1, 0, rd)) break;
            } else if (op == 0xBu) {                     /* CMN = ADDS */
                e_load_r(a, 9, rn);
                if (!emit_dp(a, 4, 1, 0, rd)) break;
            } else {
                if (!writes || (op != 0 && op != 1 && op != 2 && op != 4 &&
                                op != 0xC && op != 0xE))
                    break;
                e_load_r(a, 9, rn);
                if (!emit_dp(a, op, (int)S, writes, rd)) break;
            }
        }
        pc += 4; n++;
    }
    if (!n)
        return 0;
    e_set_pc(a, pc);
    /* Deliberately NOT linkable. A block falls out here because it reached an
     * instruction it could not lower, so the next PC is exactly that
     * instruction -- and a block compiled there would be rejected on its first
     * instruction. The link could never be satisfied, and measuring it proved
     * the point: 2 links formed across 35708 blocks. Only branch exits, whose
     * target is a real instruction, are worth a slot. */
    e_return(a, n);
    *out_count = (uint16_t)n;
    if (n >= JIT_MAX_GUEST || a->n + 48u >= JIT_MAX_A64)
        *out_end = 0;           /* filled up; nothing blocked it */
    return 1;
}

static int compile_thumb(Guest *g, A64 *a, uint32_t start, uint32_t until,
                         uint16_t *out_count, uint32_t *out_end) {
    uint32_t pc = start, n = 0;
    int verify = g->jit_verify;
    *out_end = 0;
    while (n < JIT_MAX_GUEST && a->n + 48 < JIT_MAX_A64) {
        uint32_t hw;
        if (pc == (until & ~1u) || has_hook(g, pc) ||
            !guest_ld16(&g->mem, pc, &hw)) {
            *out_end = 0;       /* see compile_arm */
            break;
        }
        *out_end = hw;

        if ((hw & 0xF800u) >= 0xE800u) {                 /* selected T32 */
            uint32_t hw2, rn, rd, imm12, kind;
            if (!guest_ld16(&g->mem, pc + 2u, &hw2)) break;

            /* Branches: hw2 bit 15 set. Both end the block, and lowering them
             * folds the branch into it rather than handing one instruction
             * back to the interpreter every time a block ends. */
            if (hw2 & 0x8000u) {
                uint32_t S1, j1, j2, i1, i2, imm11, imm10;
                int32_t off;
                if ((hw & 0xF800u) != 0xF000u) break;
                if ((hw2 & 0xD000u) == 0x8000u) {        /* conditional B.W */
                    uint32_t cond = (hw >> 6) & 0xFu;
                    uint32_t s2 = (hw >> 10) & 1u;
                    uint32_t j1 = (hw2 >> 13) & 1u, j2 = (hw2 >> 11) & 1u;
                    int32_t coff;
                    if (cond >= 0xEu) break;             /* not a condition */
                    coff = (int32_t)(((s2 << 20) | (j2 << 19) | (j1 << 18) |
                                      ((hw & 0x3Fu) << 12) |
                                      ((hw2 & 0x7FFu) << 1)) << 11) >> 11;
                    e_ldr_w(a, 10, (unsigned)offsetof(Guest, cpu.cpsr));
                    e_mov32(a, 11, 0xF0000000u);
                    e_rr(a, 0x0A000000u, 10, 10, 11);
                    emit(a, 0xD51B4200u | 10u);          /* MSR NZCV, x10 */
                    e_mov32(a, 9, pc + 4u + (uint32_t)coff);
                    e_mov32(a, 11, pc + 4u);
                    emit(a, 0x1A800000u | (11u << 16) | (cond << 12) |
                            (9u << 5) | 9u);
                    e_store_r(a, 9, GUEST_PC);
                    n++; e_return(a, n); *out_count = (uint16_t)n;
                    *out_end = 0;
                    return 1;
                }
                if ((hw2 & 0xD000u) != 0x9000u && (hw2 & 0xD000u) != 0xD000u)
                    break;
                S1 = (hw >> 10) & 1u;
                imm10 = hw & 0x3FFu;
                j1 = (hw2 >> 13) & 1u;
                j2 = (hw2 >> 11) & 1u;
                imm11 = hw2 & 0x7FFu;
                i1 = 1u - (j1 ^ S1);
                i2 = 1u - (j2 ^ S1);
                /* 25-bit signed displacement. The left shift is done on the
                 * unsigned value and only then reinterpreted: casting first
                 * and shifting a negative int32_t left is undefined. */
                off = (int32_t)(((S1 << 24) | (i1 << 23) | (i2 << 22) |
                                 (imm10 << 12) | (imm11 << 1)) << 7) >> 7;
                if ((hw2 & 0xD000u) == 0xD000u) {   /* BL: LR = return | 1 */
                    e_mov32(a, 9, (pc + 4u) | 1u);
                    e_store_r(a, 9, GUEST_LR);
                }
                e_set_pc(a, pc + 4u + (uint32_t)off);
                n++; e_return_linkable(a, n, (pc + 4u + (uint32_t)off) | 1u);
                *out_count = (uint16_t)n; *out_end = 0;
                return 1;
            }

            /* Wide load/store, 12-bit immediate (T3):
             *   1111 1000 1 size(2) L Rn   Rt imm12
             * The register-offset and imm8 forms are T4 and are not lowered. */
            if ((hw & 0xFF80u) == 0xF880u) {
                uint32_t sz = (hw >> 5) & 3u, ld = (hw >> 4) & 1u;
                uint32_t wrn = hw & 0xFu, wrt = (hw2 >> 12) & 0xFu;
                uint32_t wimm = hw2 & 0xFFFu;
                int wb = (sz == 2u) ? 4 : (sz == 1u) ? 2 : 1;
                if (sz == 3u || wrn == GUEST_PC || wrt == GUEST_PC) break;
                if (ld) {
                    e_load_r(a, 9, wrn);
                    if (wimm)
                        emit(a, 0x11000000u | (wimm << 10) | (9u << 5) | 9u);
                    e_load_mem(a, wb, pc, n);
                    e_store_r(a, 9, wrt);
                } else {
                    e_load_r(a, 14, wrt);
                    e_load_r(a, 9, wrn);
                    if (wimm)
                        emit(a, 0x11000000u | (wimm << 10) | (9u << 5) | 9u);
                    e_store_mem(a, wb, pc, n, verify);
                }
                pc += 4; n++;
                continue;
            }

            /* Data processing, modified immediate. */
            if ((hw & 0xFA00u) == 0xF000u) {
                uint32_t op = (hw >> 5) & 0xFu, S = (hw >> 4) & 1u;
                uint32_t drn = hw & 0xFu, drd = (hw2 >> 8) & 0xFu;
                uint32_t di12 = (((hw >> 10) & 1u) << 11) |
                                (((hw2 >> 12) & 7u) << 8) | (hw2 & 0xFFu);
                int rotated, aop = t32_dp_op(op);
                uint32_t val;
                int logical;
                /* Rd = 1111 with S set is how T32 spells CMP/TST/TEQ/CMN:
                 * the result is discarded and only the flags matter, so it is
                 * a compare rather than a write to PC. Without S it is an
                 * unallocated form and the block ends. */
                if (aop < 0 || (drd == GUEST_PC && !S)) break;
                logical = (aop == 0x0 || aop == 0x1 || aop == 0xC ||
                           aop == 0xE);
                val = jit_thumb_imm(di12, &rotated);
                /* A flag-setting logical takes C from the immediate's
                 * carry-out. When the immediate is not the rotated form that
                 * carry-out is the carry-in, so C is simply preserved and
                 * N/Z alone is correct; when it is, C would have to be
                 * modelled and the block ends instead. */
                if (S && logical && rotated) break;
                e_mov32(a, 10, val);
                if (drn == GUEST_PC) {
                    if (aop != 0xC || drd == GUEST_PC) break;  /* ORR -> MOV */
                    e_rr(a, 0x2A000000u, 9, 31, 10);
                    e_store_r(a, 9, drd);
                    if (S) e_set_nz(a, 9);
                } else {
                    e_load_r(a, 9, drn);
                    if (!emit_dp(a, (uint32_t)aop, (int)S,
                                 drd != GUEST_PC, drd))
                        break;
                }
                pc += 4; n++;
                continue;
            }
            if ((hw & 0xFA00u) != 0xF200u) break;
            rn = hw & 0xFu; rd = (hw2 >> 8) & 0xFu;
            imm12 = (((hw >> 10) & 1u) << 11) |
                    (((hw2 >> 12) & 7u) << 8) | (hw2 & 0xFFu);
            kind = hw & 0xFBF0u;
            if (rd == GUEST_PC) break;
            if (kind == 0xF200u || kind == 0xF2A0u) {    /* ADDW / SUBW */
                /* T4: no flags, 12-bit immediate -- exactly the width of the
                 * AArch64 ADD/SUB immediate field, so it lowers one to one.
                 * Rn = PC is the ADR form and is left alone. */
                if (rn == GUEST_PC) break;
                e_load_r(a, 9, rn);
                emit(a, (kind == 0xF200u ? 0x11000000u : 0x51000000u) |
                        (imm12 << 10) | (9u << 5) | 9u);
                e_store_r(a, 9, rd);
            } else if (kind == 0xF240u) {                 /* MOVW */
                e_mov32(a, 9, (rn << 12) | imm12);
                e_store_r(a, 9, rd);
            } else if (kind == 0xF2C0u) {                /* MOVT */
                uint32_t imm = (rn << 12) | imm12;
                e_load_r(a, 9, rd);
                emit(a, 0x72A00000u | (imm << 5) | 9u);
                e_store_r(a, 9, rd);
            } else if ((kind == 0xF200u || kind == 0xF2A0u) && rn != GUEST_PC) {
                e_load_r(a, 9, rn);                      /* ADDW / SUBW */
                e_mov32(a, 10, imm12);
                e_rr(a, kind == 0xF200u ? 0x0B000000u : 0x4B000000u,
                     9, 9, 10);
                e_store_r(a, 9, rd);
            } else break;
            pc += 4; n++;
            continue;
        }

        if ((hw & 0xE000u) == 0x0000u && ((hw >> 11) & 3u) == 3u) {
            uint32_t rd = hw & 7u, rm = (hw >> 3) & 7u, rn = (hw >> 6) & 7u;
            e_load_r(a, 9, rm);
            if (hw & 0x0400u) e_mov32(a, 10, rn); else e_load_r(a, 10, rn);
            e_rr(a, (hw & 0x0200u) ? 0x6B000000u : 0x2B000000u, 9, 9, 10);
            e_merge_nzcv(a, CPSR_N | CPSR_Z | CPSR_C | CPSR_V);
            e_store_r(a, 9, rd);
        } else if ((hw & 0xE000u) == 0x2000u) {          /* imm8 group */
            uint32_t op = (hw >> 11) & 3u, rd = (hw >> 8) & 7u;
            e_mov32(a, 10, hw & 0xFFu);
            if (op == 0) {                               /* MOVS */
                e_rr(a, 0x2A000000u, 9, 31, 10);
                e_store_r(a, 9, rd);
                e_set_nz(a, 9);
            } else {
                e_load_r(a, 9, rd);
                e_rr(a, (op == 2) ? 0x2B000000u : 0x6B000000u,
                     op == 1 ? 31u : 9u, 9, 10);
                e_merge_nzcv(a, CPSR_N | CPSR_Z | CPSR_C | CPSR_V);
                if (op != 1) e_store_r(a, 9, rd);
            }
        } else if ((hw & 0xFC00u) == 0x4400u && ((hw >> 8) & 3u) != 3u) {
            uint32_t op = (hw >> 8) & 3u;
            uint32_t rd = ((hw >> 4) & 8u) | (hw & 7u), rm = (hw >> 3) & 0xFu;
            if (rd == GUEST_PC || rm == GUEST_PC) break;
            e_load_r(a, 10, rm);
            if (op == 2) {                               /* MOV */
                e_rr(a, 0x2A000000u, 9, 31, 10);
                e_store_r(a, 9, rd);
            } else {
                e_load_r(a, 9, rd);
                e_rr(a, op == 0 ? 0x0B000000u : 0x6B000000u,
                     op == 1 ? 31u : 9u, 9, 10);
                if (op == 1) e_merge_nzcv(a, CPSR_N|CPSR_Z|CPSR_C|CPSR_V);
                else e_store_r(a, 9, rd);
            }
        } else if ((hw & 0xFE00u) == 0xBC00u ||
                   (hw & 0xFE00u) == 0xB400u) {          /* POP / PUSH */
            /* The stack half of every prologue and epilogue, and the largest
             * remaining block terminator once stores landed.
             *
             * The whole transfer range is bounds-checked ONCE, before any of
             * it happens. Checking per register looked equivalent and is not:
             * a bail on the third register of a PUSH would leave two words
             * already written, and on a POP two guest registers already
             * loaded, while reporting that the instruction never ran. The
             * interpreter then re-executes from before it and never makes
             * those writes -- which the verifier correctly reports as a
             * divergence, because it is one. One check up front means a bail
             * always happens with nothing yet done, and it is faster besides.
             *
             * SP still moves only at the end, so re-execution after a bail
             * starts from the same stack. */
            uint32_t list = hw & 0xFFu, extra = (hw >> 8) & 1u;
            uint32_t pop = (hw & 0xFE00u) == 0xBC00u;
            uint32_t cnt = extra, r, off = 0;
            unsigned mem = (unsigned)offsetof(Guest, mem);
            for (r = 0; r < 8; r++) if (list & (1u << r)) cnt++;
            if (!cnt || a->n + 10u * cnt + 80u > JIT_MAX_A64)
                break;
            e_load_r(a, 16, GUEST_SP);
            if (!pop)                                    /* PUSH: SP - 4*cnt */
                emit(a, 0x51000000u | ((cnt * 4u) << 10) | (16u << 5) | 16u);
            e_rr(a, 0x2A000000u, 9, 31, 16);             /* MOV w9,w16 */
            /* The whole transfer is still one check: the span is 4*cnt bytes
             * from the lowest address touched, so a bail happens before any
             * register or memory has changed. */
            e_resolve(a, !pop, cnt * 4u, pc, n);
            emit(a, 0x8B204000u | (9u << 16) | (12u << 5) | 12u);
                                                 /* x12 = host + offset */
            for (r = 0; r < 9; r++) {
                uint32_t greg;
                if (r < 8) {
                    if (!(list & (1u << r))) continue;
                    greg = r;
                } else {
                    if (!extra) break;
                    greg = pop ? GUEST_PC : GUEST_LR;
                }
                if (pop) {
                    emit(a, 0xB9400000u | ((off >> 2) << 10) | (12u << 5) | 9u);
                    if (greg != GUEST_PC)
                        e_store_r(a, 9, greg);           /* PC handled below */
                } else {
                    e_load_r(a, 9, greg);
                    if (verify) {
                        /* Undo record. x12 (the host base) and w9 (the value)
                         * must survive, so this uses w10, w11 and w13 only. */
                        unsigned noff = mem + (unsigned)offsetof(GuestMem, undo_n);
                        e_ldr_w(a, 10, noff);
                        e_mov32(a, 11, GUEST_UNDO_MAX);
                        e_rr(a, 0x6B000000u, 31, 10, 11);
                        emit(a, 0x54000000u | (6u << 5) | 3u);   /* B.LO +6 */
                        e_bail(a, pc, n);
                        emit(a, 0xB9400000u | ((off >> 2) << 10) | (12u << 5) | 13u);
                        e_rr(a, 0x2A000000u, 15, 31, 16);        /* w15 = base */
                        if (off)
                            emit(a, 0x11000000u | (off << 10) | (15u << 5) | 15u);
                        emit(a, 0xF9400000u |
                                (((mem + (unsigned)offsetof(GuestMem, undo_log)) >> 3) << 10) |
                                (0u << 5) | 11u);
                        emit(a, 0x8B000000u | (10u << 16) | (4u << 10) | (11u << 5) | 11u);
                        emit(a, 0xB9000000u | (0u << 10) | (11u << 5) | 15u);
                        emit(a, 0xB9000000u | (1u << 10) | (11u << 5) | 13u);
                        emit(a, 0xB9000000u | (2u << 10) | (11u << 5) | 31u);
                        e_mov32(a, 13, 4u);
                        emit(a, 0xB9000000u | (3u << 10) | (11u << 5) | 13u);
                        emit(a, 0x11000400u | (10u << 5) | 10u);
                        e_str_w(a, 10, noff);
                    }
                    emit(a, 0xB9000000u | ((off >> 2) << 10) | (12u << 5) | 9u);
                }
                off += 4u;
            }
            if (pop) {
                e_load_r(a, 10, GUEST_SP);
                emit(a, 0x11000000u | ((cnt * 4u) << 10) | (10u << 5) | 10u);
                e_store_r(a, 10, GUEST_SP);
            } else {
                e_store_r(a, 16, GUEST_SP);
            }
            if (pop && extra) {                          /* POP {..., pc} */
                e_interwork(a);                          /* w9 holds it still */
                n++; e_return(a, n); *out_count = (uint16_t)n; *out_end = 0;
                return 1;
            }
            pc += 2; n++;
            continue;
        } else if ((hw & 0xF500u) == 0xB100u) {          /* CBZ / CBNZ */
            uint32_t rn = hw & 7u;
            uint32_t off5 = (((hw >> 9) & 1u) << 5) | ((hw >> 3) & 0x1Fu);
            e_load_r(a, 10, rn);
            e_rr(a, 0x6B000000u, 31, 10, 31);            /* CMP w10,wzr */
            e_mov32(a, 9, pc + 4u + off5 * 2u);          /* taken */
            e_mov32(a, 11, pc + 2u);                     /* not taken */
            /* CBNZ (bit 11) branches when non-zero: NE. CBZ branches on EQ. */
            emit(a, 0x1A800000u | (11u << 16) |
                    (((hw & 0x0800u) ? 1u : 0u) << 12) | (9u << 5) | 9u);
            e_store_r(a, 9, GUEST_PC);
            n++; e_return(a, n); *out_count = (uint16_t)n; *out_end = 0;
            return 1;
        } else if ((hw & 0xFF00u) == 0x4700u) {          /* BX / BLX Rm */
            uint32_t rm = (hw >> 3) & 0xFu;
            if (rm == GUEST_PC) break;           /* BX PC: rare, and not this */
            if (hw & 0x0080u) {                  /* BLX: LR = return | 1 */
                e_mov32(a, 9, (pc + 2u) | 1u);
                e_store_r(a, 9, GUEST_LR);
            }
            e_load_r(a, 9, rm);
            e_interwork(a);
            n++; e_return(a, n); *out_count = (uint16_t)n; *out_end = 0;
            return 1;
        } else if ((hw & 0xFC00u) == 0x4000u) {          /* common ALU ops */
            uint32_t op = (hw >> 6) & 0xFu, rd = hw & 7u, rm = (hw >> 3) & 7u;
            e_load_r(a, 9, rd); e_load_r(a, 10, rm);
            if (op == 0 || op == 1 || op == 8 || op == 0xC || op == 0xE) {
                uint32_t base = op == 0 || op == 8 ? 0x0A000000u :
                                op == 1 ? 0x4A000000u :
                                op == 0xC ? 0x2A000000u : 0x0A200000u;
                e_rr(a, base, op == 8 ? 11u : 9u, 9, 10);
                e_set_nz(a, op == 8 ? 11u : 9u);
                if (op != 8) e_store_r(a, 9, rd);
            } else if (op == 9 || op == 0xA || op == 0xB) {
                if (op == 9) {                           /* NEG = 0-b */
                    e_rr(a, 0x6B000000u, 9, 31, 10);
                    e_store_r(a, 9, rd);
                } else {
                    e_rr(a, op == 0xA ? 0x6B000000u : 0x2B000000u,
                         31, 9, 10);
                }
                e_merge_nzcv(a, CPSR_N|CPSR_Z|CPSR_C|CPSR_V);
            } else if (op == 0xD) {                      /* MULS: N/Z only */
                emit(a, 0x1B007C00u | (10u << 16) | (9u << 5) | 9u);
                e_store_r(a, 9, rd); e_set_nz(a, 9);
            } else if (op == 0xF) {                      /* MVN */
                e_rr(a, 0x2A200000u, 9, 31, 10);
                e_store_r(a, 9, rd); e_set_nz(a, 9);
            } else break;
        } else if ((hw & 0xF000u) == 0xA000u) {          /* ADR / ADD SP */
            uint32_t rd = (hw >> 8) & 7u, imm = (hw & 0xFFu) * 4u;
            if (hw & 0x0800u) {
                e_load_r(a, 9, GUEST_SP); e_mov32(a, 10, imm);
                e_rr(a, 0x0B000000u, 9, 9, 10);
            } else e_mov32(a, 9, ((pc + 4u) & ~3u) + imm);
            e_store_r(a, 9, rd);
        } else if ((hw & 0xFF00u) == 0xB000u) {          /* ADD/SUB SP */
            uint32_t imm = (hw & 0x7Fu) * 4u;
            e_load_r(a, 9, GUEST_SP); e_mov32(a, 10, imm);
            e_rr(a, (hw & 0x80u) ? 0x4B000000u : 0x0B000000u, 9, 9, 10);
            e_store_r(a, 9, GUEST_SP);
        } else if ((hw & 0xF000u) == 0x6000u || (hw & 0xF000u) == 0x8000u ||
                   (hw & 0xF000u) == 0x7000u || (hw & 0xF000u) == 0x9000u ||
                   (hw & 0xF800u) == 0x4800u) {
            /* Thumb loads. This is the omission that actually mattered: the
             * Thumb compiler had no memory lowering at all, so in Thumb code
             * -- which is where gameplay lives -- a block ended at the first
             * load or store, and no amount of lowering arithmetic moved the
             * average off 1.6 instructions.
             *
             *   0110 1 LDR  Rd,[Rn,#imm5*4]      0111 1 LDRB Rd,[Rn,#imm5]
             *   1000 1 LDRH Rd,[Rn,#imm5*2]      1001 1 LDR  Rd,[SP,#imm8*4]
             *   0100 1 LDR  Rd,[PC,#imm8*4]      (literal pool)
             *
             * Stores share these encodings with the load bit clear and are
             * deliberately still rejected: verification re-runs each block, so
             * a block that writes memory needs the undo log first. */
            uint32_t rd, rn, imm, bytes = 4;
            if ((hw & 0xF800u) == 0x4800u) {         /* LDR literal */
                rd = (hw >> 8) & 7u;
                e_mov32(a, 9, ((pc + 4u) & ~3u) + (hw & 0xFFu) * 4u);
            } else if ((hw & 0xF800u) == 0x9000u) {  /* STR [SP,#imm8*4] */
                rd = (hw >> 8) & 7u;
                e_load_r(a, 14, rd);
                e_load_r(a, 9, GUEST_SP);
                imm = (hw & 0xFFu) * 4u;
                if (imm)
                    emit(a, 0x11000000u | (imm << 10) | (9u << 5) | 9u);
                e_store_mem(a, 4, pc, n, verify);
                pc += 2; n++;
                continue;
            } else if ((hw & 0xF800u) == 0x9800u) {  /* LDR [SP,#imm8*4] */
                rd = (hw >> 8) & 7u;
                e_load_r(a, 9, GUEST_SP);
                imm = (hw & 0xFFu) * 4u;
                if (imm)
                    emit(a, 0x11000000u | (imm << 10) | (9u << 5) | 9u);
            } else {
                rd = hw & 7u;
                rn = (hw >> 3) & 7u;
                imm = (hw >> 6) & 0x1Fu;
                if ((hw & 0xF000u) == 0x6000u)      imm *= 4u;
                else if ((hw & 0xF000u) == 0x8000u) { imm *= 2u; bytes = 2; }
                else                                  bytes = 1;
                if (!(hw & 0x0800u)) {                /* store */
                    e_load_r(a, 14, rd);
                    e_load_r(a, 9, rn);
                    if (imm)
                        emit(a, 0x11000000u | (imm << 10) | (9u << 5) | 9u);
                    e_store_mem(a, (int)bytes, pc, n, verify);
                    pc += 2; n++;
                    continue;
                }
                e_load_r(a, 9, rn);
                if (imm)
                    emit(a, 0x11000000u | (imm << 10) | (9u << 5) | 9u);
            }
            e_load_mem(a, (int)bytes, pc, n);
            e_store_r(a, 9, rd);
        } else if ((hw & 0xF000u) == 0xD000u &&
                   ((hw >> 8) & 0xFu) < 0xEu) {          /* conditional branch */
            /* Ends the block either way, so it costs nothing to get both
             * successors right: compute the target and the fall-through, then
             * pick between them with the guest's own flags.
             *
             * ARM and AArch64 condition codes share an encoding, so the guest
             * condition can be used verbatim -- the only work is getting the
             * guest's NZCV into the host's, which MSR does directly. Masking
             * first because the rest of CPSR is not flags and NZCV's low bits
             * are RES0. */
            uint32_t cond = (hw >> 8) & 0xFu;
            int32_t off = (int32_t)((hw & 0xFFu) << 24) >> 23;
            e_ldr_w(a, 10, (unsigned)offsetof(Guest, cpu.cpsr));
            e_mov32(a, 11, 0xF0000000u);
            e_rr(a, 0x0A000000u, 10, 10, 11);            /* AND w10,w10,w11 */
            emit(a, 0xD51B4200u | 10u);                  /* MSR NZCV, x10 */
            e_mov32(a, 9, pc + 4u + (uint32_t)off);      /* taken */
            e_mov32(a, 11, pc + 2u);                     /* not taken */
            emit(a, 0x1A800000u | (11u << 16) | (cond << 12) |
                    (9u << 5) | 9u);                     /* CSEL w9,w9,w11,cond */
            e_store_r(a, 9, GUEST_PC);
            n++; e_return(a, n); *out_count = (uint16_t)n; *out_end = 0;
            return 1;
        } else if ((hw & 0xF800u) == 0xE000u) {          /* B */
            int32_t off = (int32_t)((hw & 0x7FFu) << 21) >> 20;
            e_set_pc(a, pc + 4u + (uint32_t)off);
            n++; e_return_linkable(a, n, (pc + 4u + (uint32_t)off) | 1u);
            *out_count = (uint16_t)n; *out_end = 0;
            return 1;
        } else break;
        pc += 2; n++;
    }
    if (!n) return 0;
    e_set_pc(a, pc); e_return(a, n); *out_count = (uint16_t)n;   /* see above */
    if (n >= JIT_MAX_GUEST || a->n + 48u >= JIT_MAX_A64)
        *out_end = 0;           /* filled up; nothing blocked it */
    return 1;
}

/* Has this PC been seen JIT_HOT_COUNT times?
 *
 * This exists because claiming a block-table slot on the FIRST sighting of a
 * PC is what held coverage to ~1%. Slots are never released, so one-shot
 * startup and loader code filled all of them by instruction ~816k -- and the
 * first frame is at 743M. From then on lookup() failed its probe walk for
 * every new PC and the JIT declined the entire game, having compiled nothing
 * but the loader. Simulating the table over real runs measured the reachable
 * share of the instruction stream at 0.85% after 300M instructions and 0.08%
 * after 5B: it got worse the longer the game ran, because the table had frozen
 * before the game started.
 *
 * Counting sightings here instead costs 64 KB and no slot. Cold code churns
 * this array and never touches the block table; the same simulation puts the
 * reachable share at 99.89%. Direct-mapped and tagged rather than hashed with
 * probing, because this runs on every declined PC and must stay a handful of
 * instructions: a tag mismatch simply takes the entry over, so a PC that is
 * genuinely hot re-hits and promotes anyway, while a PC that is not loses
 * nothing worth keeping.
 *
 * The hits==0 test matters: an untouched array is all zeroes, so without it a
 * PC whose tag is 0 would read a fresh entry as an established one. */
static int jit_hot(JitContext *j, uint32_t key) {
    uint32_t h = key * 2654435761u;
    JitCount *c = &j->count[h & (JIT_HOT_SLOTS - 1u)];
    uint16_t tag = (uint16_t)(h >> 16);
    if (c->tag != tag || !c->hits) {
        c->tag = tag;
        c->hits = 1;
        return 0;
    }
    if (c->hits < 255u)
        c->hits++;
    return c->hits >= JIT_HOT_COUNT;
}

/* Indexed by the PC itself, not a hash of it.
 *
 * A multiplicative hash is the right choice when keys are arbitrary; these are
 * program counters, which are dense, local, and executed in runs. Hashing them
 * spreads a hot loop's blocks uniformly across 65536 slots and guarantees the
 * table behaves like its full size. Shifting off the Thumb bit and taking the
 * low bits instead keeps neighbouring PCs in neighbouring slots, so a hot loop
 * touches a handful of lines rather than a handful of pages. Linear probing is
 * unchanged, so entries are still never displaced once claimed. */
static JitEntry *lookup(JitContext *j, uint32_t key) {
    uint32_t i = (key >> 1) & (JIT_CACHE_SLOTS - 1u), n;
    for (n = 0; n < 16; n++, i = (i + 1u) & (JIT_CACHE_SLOTS - 1u)) {
        if (!j->entry[i].state || j->entry[i].key == key)
            return &j->entry[i];
    }
    return NULL;
}

static int compile_block(Guest *g, JitContext *j, JitEntry *e, uint32_t until) {
    A64 a;
    uint16_t count = 0;
    uint32_t bytes, off;
    Result rc;
    uint32_t end_insn = 0;
    int thumb = guest_is_thumb(&g->cpu);
    memset(&a, 0, sizeof a);
    if (g->cpu.itstate || !(thumb
          ? compile_thumb(g, &a, g->cpu.r[GUEST_PC], until, &count, &end_insn)
          : compile_arm(g, &a, g->cpu.r[GUEST_PC], until, &count, &end_insn))) {
        e->state = 3;
        return 0;
    }
    j->cold[e - j->entry].end_insn = end_insn;
    j->cold[e - j->entry].end_thumb = (uint8_t)thumb;
    /* A block that lowered nothing is worse than no block: it stays in the
     * cache as "compiled", so every visit to this PC pays an indirect call
     * into generated code that immediately returns zero, and the interpreter
     * then does all the work anyway. Reject the PC instead. */
    if (!count) {
        e->state = 3;
        return 0;
    }
    bytes = a.n * 4u;
    off = (j->used + 15u) & ~15u;
    if (off + bytes > JIT_CODE_SIZE) {
        printf("  [jit  ] code cache full at %u KB\n", j->used >> 10);
        e->state = 3;
        return 0;
    }
    rc = jitTransitionToWritable(&j->code);
    if (R_FAILED(rc)) { e->state = 3; return 0; }
    memcpy(j->rw + off, a.code, bytes);
    armDCacheFlush(j->rw + off, bytes);
    rc = jitTransitionToExecutable(&j->code);
    if (R_FAILED(rc)) { e->state = 3; return 0; }
    armICacheInvalidate(j->rx + off, bytes);
    e->rx_off = off;
    e->count = count;
    e->state = 2;
    /* Word index of the chain slot within the cache, not within the block. */
    {
        JitCold *cd = &j->cold[e - j->entry];
        cd->link_word = a.link_word ? (off / 4u) + a.link_word : 0u;
        cd->link_key = a.link_key;
        /* "linked" doubles as "nothing left to install", so a block with no
         * link slot never costs the dispatcher a second look. */
        e->linked = cd->link_word ? 0u : 1u;
    }
    j->used = off + bytes;
    g->jit_blocks++;
    if (g->jit_blocks == 1 || g->jit_blocks == 100 ||
        (g->jit_blocks % 1000u) == 0)
        printf("  [jit  ] %u blocks, %u KB code\n",
               g->jit_blocks, j->used >> 10);
    return 1;
}

/* The one piece of generated code C calls directly.
 *
 * Blocks keep their retired count in w19 and the remaining budget in w20, so a
 * chain of them adds up without returning to C. Both are callee-saved, which
 * is exactly why they are usable here and exactly why they have to be saved:
 * the C caller expects them preserved. Every block RETs to this frame,
 * including one reached through a chain -- the chain branches are plain B, so
 * the link register still points here from the original BLR.
 *
 * Returns the total guest instructions retired across the whole chain. */
static uint32_t emit_enter(A64 *a) {
    emit(a, 0xA9800000u | ((0x7Cu) << 15) | (20u << 10) | (31u << 5) | 19u);
                                        /* STP x19,x20,[sp,#-32]! */
    emit(a, 0xF9000000u | (2u << 10) | (31u << 5) | 30u);   /* STR x30,[sp,#16] */
    emit(a, 0x52800000u | (0u << 5) | 19u);                 /* MOV w19,#0 */
    emit(a, 0x2A000000u | (2u << 16) | (31u << 5) | 20u);   /* MOV w20,w2 */
    emit(a, 0xD63F0020u);                                   /* BLR x1 */
    emit(a, 0x2A000000u | (19u << 16) | (31u << 5) | 0u);   /* MOV w0,w19 */
    emit(a, 0xF9400000u | (2u << 10) | (31u << 5) | 30u);   /* LDR x30,[sp,#16] */
    emit(a, 0xA8C00000u | (4u << 15) | (20u << 10) | (31u << 5) | 19u);
                                        /* LDP x19,x20,[sp],#32 */
    emit(a, 0xD65F03C0u);                                   /* RET */
    return a->n;
}

/* Point `from`'s chain slot at `to`. One word, but it is RX memory, so it
 * costs a writable/executable transition pair and the cache maintenance that
 * goes with them. Links are installed once per edge and there are only as many
 * edges as blocks, so the cost is bounded and paid during warm-up. */
static void jit_link(JitContext *j, JitEntry *from, JitEntry *to) {
    uint32_t link_word = j->cold[from - j->entry].link_word;
    uint32_t *slot_rw = (uint32_t *)j->rw + link_word;
    int32_t delta = (int32_t)(to->rx_off / 4u) - (int32_t)link_word;
    Result rc;
    if (delta > 0x01FFFFFF || delta < -0x02000000)
        return;                         /* out of B range; leave it returning */
    rc = jitTransitionToWritable(&j->code);
    if (R_FAILED(rc))
        return;
    *slot_rw = 0x14000000u | ((uint32_t)delta & 0x03FFFFFFu);
    armDCacheFlush(slot_rw, 4);
    rc = jitTransitionToExecutable(&j->code);
    if (R_FAILED(rc))
        return;
    armICacheInvalidate((uint32_t *)j->rx + link_word, 4);
    from->linked = 1;
    j->links++;
}

int guest_jit_init(Guest *g) {
    JitContext *j;
    Result rc;
    if (g->jit) return 1;
    j = (JitContext *)calloc(1, sizeof *j);
    if (!j) return 0;
    j->entry = (JitEntry *)calloc(JIT_CACHE_SLOTS, sizeof *j->entry);
    if (!j->entry) { free(j); return 0; }
    j->cold = (JitCold *)calloc(JIT_CACHE_SLOTS, sizeof *j->cold);
    if (!j->cold) { free(j->entry); free(j); return 0; }
    j->count = (JitCount *)calloc(JIT_HOT_SLOTS, sizeof *j->count);
    if (!j->count) { free(j->cold); free(j->entry); free(j); return 0; }
    rc = jitCreate(&j->code, JIT_CODE_SIZE);
    if (R_FAILED(rc)) {
        printf("  [jit  ] unavailable (jitCreate=%08x), interpreter only\n", rc);
        free(j->count); free(j->cold); free(j->entry); free(j); return 0;
    }
    j->rw = (uint8_t *)jitGetRwAddr(&j->code);
    j->rx = (uint8_t *)jitGetRxAddr(&j->code);
    j->ready = 1;
    g->jit = j;
    printf("  [jit  ] AArch64 cache ready: %u MB, type %d\n",
           JIT_CODE_SIZE >> 20, (int)j->code.type);
    /* First hardware gate: use exactly the two-instruction leaf from libnx's
     * JIT example. Do not run translated guest blocks in this build. The prior
     * merged NRO died during early startup; this distinguishes a Horizon W^X
     * problem from a bad ARM lowering without discarding the cache/emitter. */
    {
        static const uint32_t probe[2] = { 0xD28000E0u, 0xD65F03C0u };
        uint32_t (*probe_fn)(void) = NULL;
        void *probe_rx = j->rx;
        uint32_t got;
        rc = jitTransitionToWritable(&j->code);
        if (R_FAILED(rc)) {
            printf("  [jit  ] writable transition failed: %08x\n", rc);
            guest_jit_close(g);
            return 0;
        }
        memcpy(j->rw, probe, sizeof probe);
        armDCacheFlush(j->rw, sizeof probe);
        rc = jitTransitionToExecutable(&j->code);
        if (R_FAILED(rc)) {
            printf("  [jit  ] executable transition failed: %08x\n", rc);
            guest_jit_close(g);
            return 0;
        }
        armICacheInvalidate(j->rx, sizeof probe);
        memcpy(&probe_fn, &probe_rx, sizeof probe_fn);
        got = probe_fn();
        printf("  [jit  ] W^X probe returned %u (expected 7)\n",
               (unsigned)got);
        if (got != 7u) {
            guest_jit_close(g);
            return 0;
        }
        j->used = 16u;
        j->probe_only = 0;
    }
    {   /* The trampoline is emitted once, at the head of the cache. */
        A64 t;
        uint32_t bytes;
        memset(&t, 0, sizeof t);
        emit_enter(&t);
        bytes = t.n * 4u;
        rc = jitTransitionToWritable(&j->code);
        if (R_FAILED(rc)) { guest_jit_close(g); return 0; }
        memcpy(j->rw + j->used, t.code, bytes);
        armDCacheFlush(j->rw + j->used, bytes);
        rc = jitTransitionToExecutable(&j->code);
        if (R_FAILED(rc)) { guest_jit_close(g); return 0; }
        armICacheInvalidate(j->rx + j->used, bytes);
        j->enter_off = j->used;
        j->used += bytes;
    }
    return 1;
}

void guest_jit_close(Guest *g) {
    JitContext *j = (JitContext *)g->jit;
    if (!j) return;
    jitClose(&j->code);
    free(j->count); free(j->cold); free(j->entry); free(j); g->jit = NULL;
}

/* r16 hardware trial: enable the deliberately narrow tier automatically. */
/* What is stopping blocks from being longer, ranked by the executions it costs.
 *
 * Reported per encoding rather than per reason: the encoding is what has to be
 * lowered next, and naming it directly removes a step of interpretation. ARM
 * rows are keyed by bits 20-27, Thumb rows by the top byte of the halfword --
 * the same keys the interpreter's instruction mix uses, so the two reports can
 * be read side by side. */
void guest_jit_report_blockers(Guest *g) {
    JitContext *j = (JitContext *)g->jit;
    static uint64_t weight[512];
    static uint32_t worst_pc[512];
    static uint32_t worst_ex[512];
    static uint32_t worst_insn[512];
    uint32_t i, k;
    uint64_t total = 0;

    if (!j || !j->entry)
        return;
    memset(weight, 0, sizeof weight);
    memset(worst_pc, 0, sizeof worst_pc);
    memset(worst_ex, 0, sizeof worst_ex);
    memset(worst_insn, 0, sizeof worst_insn);
    for (i = 0; i < JIT_CACHE_SLOTS; i++) {
        JitEntry *e = &j->entry[i];
        JitCold *cd = &j->cold[i];
        uint32_t idx;
        if (e->state != 2 || !cd->execs || !cd->end_insn)
            continue;
        /* Thumb encodes immediate bits in the top byte, so keying on it
         * splits one instruction class across eight rows and makes each look
         * small: STR is 0x60-0x67 and LDR is 0x68-0x6F. That is how "no Thumb
         * load/store lowering at all" hid behind a 20% row. Key on the major
         * opcode instead, and report it as a representative encoding. */
        idx = cd->end_thumb ? ((cd->end_insn >> 11) & 0x1Fu)
                            : (256u + ((cd->end_insn >> 20) & 0xFFu));
        weight[idx] += cd->execs;
        total += cd->execs;
        /* Keep the busiest block in each bucket. A bucket is four hex digits
         * of an encoding and several different instructions share one -- ADR
         * and a jump-table "ADD pc,rn,#imm" both land in ARM 0028 -- so the
         * bucket alone has repeatedly sent me lowering the wrong thing. An
         * address can just be disassembled. */
        if (cd->execs > worst_ex[idx]) {
            worst_ex[idx] = cd->execs;
            worst_pc[idx] = e->key & ~1u;
            worst_insn[idx] = cd->end_insn;
        }
    }
    g->jit_links = j->links;
    printf("  [jit  ] %u chain links installed, chaining %s\n",
           j->links, j->chain ? "on" : "off");
    if (!total)
        return;
    printf("  [jitb ] what ends blocks, by executions cost:\n");
    for (k = 0; k < 8; k++) {
        uint64_t best = 0;
        uint32_t bi = 0;
        for (i = 0; i < 512; i++)
            if (weight[i] > best) { best = weight[i]; bi = i; }
        if (!best)
            break;
        printf("  [jitb ]   %-3s %04x %2llu%% %8llu  worst block %08x ends on %08x\n",
               bi < 256 ? "T16" : "ARM",
               bi < 256 ? (unsigned)(bi << 11) : (unsigned)(bi & 0xFFu),
               (unsigned long long)(best * 100ull / total),
               (unsigned long long)best,
               (unsigned)worst_pc[bi], (unsigned)worst_insn[bi]);
        weight[bi] = 0;
    }
}

static int jit_wanted(void) {
    static int decided;
    if (!decided) {
        decided = 1;
        printf("  [jit  ] tier-1 hardware trial enabled\n");
    }
    return 1;
}

int guest_jit_try_run(Guest *g, uint32_t until, uint64_t remaining,
                      uint32_t *retired) {
    JitContext *j;
    JitEntry *e;
    uint32_t key, n;
    JitBlockFn fn;
    *retired = 0;
    if (!jit_wanted())
        return 0;
    if (remaining < 2 || g->cpu.itstate || guest_is_stub(g->cpu.r[GUEST_PC]))
        return 0;
    if (!g->jit && !guest_jit_init(g)) return 0;
    j = (JitContext *)g->jit;
    if (j->probe_only)
        return 0;
    j->chain = g->jit_chain && !g->jit_verify;
    g_fastmem = g->mem.fast_base != NULL;
    key = g->cpu.r[GUEST_PC] | (guest_is_thumb(&g->cpu) ? 1u : 0u);
    e = lookup(j, key);
    if (!e) return 0;
    /* An empty slot is left empty until the PC has proved it is worth one.
     * lookup() returns the first empty slot in the probe chain, and entries
     * are never released, so a key can never sit beyond an empty slot in its
     * own chain -- declining to claim this one cannot hide an existing block.
     * A slot already claimed skips the filter entirely, so a compiled block
     * stays reachable even if a tag collision later evicts its counter. */
    if (!e->state) {
        if (!jit_hot(j, key)) return 0;
        e->key = key;
        e->state = 1;
    }
    if (e->state == 3) return 0;
    if (e->state == 1 && !compile_block(g, j, e, until)) return 0;
    if (remaining < e->count) return 0;
    /* Install the edge the dispatcher just observed.
     *
     * Links are formed from real transitions rather than guessed at compile
     * time: the predecessor left on a constant PC, control arrived here, and
     * this block is compiled -- so the edge is real and worth a branch. Never
     * link to a PC carrying a hook (an observe hook falls through to the JIT,
     * and a chain would silently skip it), and never while verifying, because
     * the verifier re-runs one block at a time and a chain is not one block. */
    /* `linked` is the hot flag and is set for blocks with no slot too, so the
     * common case costs one byte already in the line the dispatch just read.
     * Only an unlinked block with something to install reaches the cold half. */
    if (j->chain && j->last && !j->last->linked && !g->jit_verify) {
        JitCold *lc = &j->cold[j->last - j->entry];
        if (lc->link_key == key && !has_hook(g, key & ~1u))
            jit_link(j, j->last, e);
    }
    {
        JitEnterFn enter;
        void *blk = j->rx + e->rx_off;
        /* Capped so w20 stays comfortably positive: the per-block decrement is
         * an imm12 and the chain test is signed. */
        uint32_t budget = remaining > 0x00FFFFFFull ? 0x00FFFFFFu
                                                    : (uint32_t)remaining;
        memcpy(&enter, &(void *){j->rx + j->enter_off}, sizeof enter);
        g_native_stage = 200;
        n = enter(g, blk, budget);
        g_native_stage = 201;
        j->last = e;
        if (n > budget) n = 0;      /* cannot happen; refuse to trust it */
    }
    (void)fn;
    /* Once loads could bail mid-block this stopped being "the leaf always
     * returns its count". A short count is a region-check miss: the block ran,
     * updated the registers it got to, and left PC on the instruction it could
     * not do. Reporting that as "did not run" threw away real work -- the
     * instructions were not counted, and with verification on they were never
     * checked either, so precisely the new code path was the one going
     * untested. */
    /* A block whose FIRST instruction bails returns zero, and until now that
     * was counted nowhere -- indistinguishable from a PC the JIT never had a
     * block for. It is not the same thing at all: the block exists, was
     * entered, and gave the whole thing back, which is the shape a
     * region-cache miss on a leading load takes. Count it separately, so the
     * next bottleneck can be read off the report rather than guessed at. */
    if (!n || (!j->chain && n > e->count)) {
        if (!n) {
            g->jit_bails++;
            g->jit_bails_empty++;
            g->jit_bail_lost += e->count;
        }
        return 0;
    }
    if (!j->chain && n < e->count) {
        g->jit_bails++;
        g->jit_bail_lost += e->count - n;
    }
    *retired = n;
    /* Per-execution counter, and it lives in the cold half: touching it on
     * every dispatch would pull a second line in and undo the split. It exists
     * only to weight the blocker report, so it is kept while profiling. */
    if (g->pcprof)
        j->cold[e - j->entry].execs++;
    g->jit_executed += n;
    return 1;
}

#endif
