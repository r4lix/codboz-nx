/* interp.c -- ARMv7-A interpreter (ARM + Thumb-2).
 *
 * Bring-up posture: anything not implemented returns GUEST_FAULT_UNDEF with the
 * raw encoding and PC recorded, so gaps announce themselves precisely instead
 * of corrupting state quietly. Fill them in frequency order -- README.md has
 * the measured histogram; ~90 Thumb-2 mnemonics cover 99%.
 *
 * PC convention: cpu.r[15] always holds the address of the instruction being
 * executed. The architectural read value (+8 ARM, +4 Thumb) is applied only
 * where an instruction actually reads PC as an operand.
 */

#include <math.h>
#include <stdio.h>      /* the JIT self-check reports divergences */
#include <string.h>

#include "guest.h"

/* --------------------------------------------------------------------- flags */

static void set_nz(GuestCpu *c, uint32_t v) {
    c->cpsr &= ~(CPSR_N | CPSR_Z);
    if (v & 0x80000000u)
        c->cpsr |= CPSR_N;
    if (v == 0)
        c->cpsr |= CPSR_Z;
}

static void set_c(GuestCpu *c, int bit) {
    c->cpsr = bit ? (c->cpsr | CPSR_C) : (c->cpsr & ~CPSR_C);
}

static void set_v(GuestCpu *c, int bit) {
    c->cpsr = bit ? (c->cpsr | CPSR_V) : (c->cpsr & ~CPSR_V);
}

static int get_c(const GuestCpu *c) { return (c->cpsr & CPSR_C) != 0; }

/* result = a + b + cin. Subtraction is a + ~b + 1, which is why SUB/CMP below
 * pass an inverted operand rather than having their own path. */
static uint32_t add_with_carry(GuestCpu *c, uint32_t a, uint32_t b, int cin,
                               int setflags) {
    uint64_t u = (uint64_t)a + (uint64_t)b + (uint64_t)cin;
    int64_t s = (int64_t)(int32_t)a + (int64_t)(int32_t)b + (int64_t)cin;
    uint32_t r = (uint32_t)u;
    if (setflags) {
        set_nz(c, r);
        set_c(c, (int)(u >> 32));
        set_v(c, s != (int64_t)(int32_t)r);
    }
    return r;
}

/* --------------------------------------------------------------------- shift */

enum { SH_LSL = 0, SH_LSR = 1, SH_ASR = 2, SH_ROR = 3 };

static uint32_t shift_c(uint32_t v, int type, int amount, int cin, int *cout) {
    *cout = cin;
    if (amount == 0)
        return v;
    switch (type) {
    case SH_LSL:
        if (amount >= 32) {
            *cout = (amount == 32) ? (int)(v & 1) : 0;
            return 0;
        }
        *cout = (int)((v >> (32 - amount)) & 1);
        return v << amount;
    case SH_LSR:
        if (amount >= 32) {
            *cout = (amount == 32) ? (int)(v >> 31) : 0;
            return 0;
        }
        *cout = (int)((v >> (amount - 1)) & 1);
        return v >> amount;
    case SH_ASR:
        if (amount >= 32) {
            *cout = (int)(v >> 31);
            return (v & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        }
        *cout = (int)(((int32_t)v >> (amount - 1)) & 1);
        return (uint32_t)((int32_t)v >> amount);
    default: {
        int a = amount & 31;
        uint32_t r = a ? ((v >> a) | (v << (32 - a))) : v;
        *cout = (int)(r >> 31);
        return r;
    }
    }
}

/* ARM modified immediate: 8-bit value rotated right by 2*rot. */
static uint32_t arm_imm(uint32_t insn, int cin, int *cout) {
    uint32_t v = insn & 0xFF;
    int rot = (int)((insn >> 8) & 0xF) * 2;
    if (!rot) {
        *cout = cin;
        return v;
    }
    v = (v >> rot) | (v << (32 - rot));
    *cout = (int)(v >> 31);
    return v;
}

/* ----------------------------------------------------------------- condition */

static int cond_ok(uint32_t cpsr, uint32_t cond) {
    int n = (cpsr & CPSR_N) != 0, z = (cpsr & CPSR_Z) != 0;
    int c = (cpsr & CPSR_C) != 0, v = (cpsr & CPSR_V) != 0;
    switch (cond & 0xF) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    default:  return 1;
    }
}

/* Writing PC from a load or register move switches execution state on bit 0. */
static void branch_interworking(GuestCpu *c, uint32_t target) {
    if (target & 1)
        c->cpsr |= CPSR_T;
    else
        c->cpsr &= ~CPSR_T;
    c->r[15] = target & ~1u;
}

#define UNDEF(g, at, enc)                                                      \
    do {                                                                       \
        (g)->undef_pc = (at);                                                  \
        (g)->undef_insn = (enc);                                               \
        return GUEST_FAULT_UNDEF;                                              \
    } while (0)

#define MEMFAULT(g, addr)                                                      \
    do {                                                                       \
        (g)->fault_addr = (addr);                                              \
        return GUEST_FAULT_MEM;                                                \
    } while (0)

/* Defined below with the rest of the VFP support; ARM and Thumb share it. */
static GuestStatus step_vfp(Guest *g, uint32_t pc, uint32_t hw, uint32_t hw2,
                            int arm);

/* ---------------------------------------------------------------- ARM decode */

static GuestStatus step_arm(Guest *g) {
    GuestCpu *c = &g->cpu;
    uint32_t pc = c->r[15], insn;

    if (!guest_ifetch32(&g->mem, pc, &insn))
        MEMFAULT(g, pc);
    if (g->iprof)
        g->iprof[512u + ((insn >> 20) & 0xFFu)]++;

    uint32_t cond = insn >> 28;
    if (cond != 0xF && !cond_ok(c->cpsr, cond)) {
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    const uint32_t read_pc = pc + 8;

    /* cond==1111 is not "always execute": it is a separate encoding group, and
     * letting it fall through is silent corruption. BLX(imm) decodes as a plain
     * B (no LR, no state switch) and PLD decodes as a load with Rd=15, which
     * writes PC. Anything here we do not know is deliberately UNDEF rather than
     * a no-op -- a loud stop is diagnosable, a skipped instruction is not. */
    if (cond == 0xF) {
        if ((insn & 0xFE000000u) == 0xFA000000u) {          /* BLX (immediate) */
            int32_t off = (int32_t)(insn << 8) >> 6;
            c->r[14] = pc + 4;
            /* Always switches to Thumb; H (bit 24) supplies the odd halfword. */
            branch_interworking(c, (read_pc + (uint32_t)off +
                                    (((insn >> 24) & 1u) << 1)) | 1u);
            return GUEST_OK;
        }
        /* Hints and barriers: architecturally no-ops for one interpreted core.
         * PLD/PLDW (imm and reg), PLI (imm and reg), CLREX/DSB/DMB/ISB. */
        if ((insn & 0xFF10F000u) == 0xF510F000u ||
            (insn & 0xFF10F010u) == 0xF710F000u ||
            (insn & 0xFF70F000u) == 0xF450F000u ||
            (insn & 0xFF70F010u) == 0xF650F000u ||
            (insn & 0xFFFFFF00u) == 0xF57FF000u) {
            c->r[15] = pc + 4;
            return GUEST_OK;
        }
        UNDEF(g, pc, insn);
    }

    /* Load/store and data-processing are the overwhelming majority of what
     * this image executes, and they sat at positions 13 and 19 of a 22-test
     * linear chain -- every one of them paid for eighteen mask compares that
     * could not match. Test them first.
     *
     * Both guards are exact, not approximate, so nothing that an earlier test
     * used to claim can be stolen here:
     *
     *  - load/store already excludes the media space (bit25 && bit4), which is
     *    every earlier test in its 01xx encoding group (REV, UADD8, SEL, BFI,
     *    SXT/UXT, SBFX and friends all set both bits).
     *  - data-processing excludes the miscellaneous space (S==0 with op 10xx,
     *    i.e. BX/BLX/CLZ/MSR/MRS, and equally MOVW/MOVT) and the multiply and
     *    extra-load/store space (bit25==0 with bits 7 and 4 set). Those are
     *    exactly the earlier tests in the 00xx group. Without the exclusions
     *    the block below would UNDEF a BX or run a MUL as TST.
     *
     * Everything else falls through to the chain in its original order, so the
     * ordering constraints documented there still hold. */
    if ((insn & 0x0C000000u) == 0x04000000u &&
        !((insn & 0x02000000u) && (insn & 0x10u)))
        goto arm_load_store;
    if ((insn & 0x0C000000u) == 0x00000000u &&
        (insn & 0x01900000u) != 0x01000000u &&
        !((insn & 0x02000000u) == 0u && (insn & 0x90u) == 0x90u))
        goto arm_data_processing;

    if ((insn & 0x0FFFFFF0u) == 0x012FFF10u) {              /* BX Rm */
        branch_interworking(c, c->r[insn & 0xF]);
        return GUEST_OK;
    }
    if ((insn & 0x0FFFFFF0u) == 0x012FFF30u) {              /* BLX Rm */
        uint32_t t = c->r[insn & 0xF];
        c->r[14] = pc + 4;
        branch_interworking(c, t);
        return GUEST_OK;
    }

    /* Coprocessor space = VFP. Thumb-2 encodes these identically to ARM with
     * cond=AL, so the Thumb decoder handles them verbatim once the condition
     * field is normalised -- step_arm has already applied the condition. */
    if ((insn & 0x0C000000u) == 0x0C000000u) {
        uint32_t a = (insn & 0x0FFFFFFFu) | 0xE0000000u;
        return step_vfp(g, pc, a >> 16, a & 0xFFFFu, 1);
    }

    if ((insn & 0x0FFF0FF0u) == 0x016F0F10u) {              /* CLZ Rd, Rm */
        uint32_t v = c->r[insn & 0xF], n = 0;
        while (n < 32 && !(v & 0x80000000u)) {
            v <<= 1;
            n++;
        }
        c->r[(insn >> 12) & 0xF] = n;
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    if ((insn & 0x0E000000u) == 0x0A000000u) {              /* B / BL */
        int32_t off = (int32_t)(insn << 8) >> 6;
        if (insn & 0x01000000u)
            c->r[14] = pc + 4;
        c->r[15] = read_pc + (uint32_t)off;
        return GUEST_OK;
    }

    /* REV / REV16 / RBIT / REVSH -- the A32 forms; the Thumb 16-bit ones are
     * handled in step_thumb. A survey of the ARM CRT says this plus UADD8/SEL
     * is the whole of the media space it actually reaches. */
    if ((insn & 0x0FBF0FF0u) == 0x06BF0F30u ||              /* REV / RBIT */
        (insn & 0x0FBF0FF0u) == 0x06BF0FB0u) {              /* REV16 / REVSH */
        uint32_t rd = (insn >> 12) & 0xFu, v = c->r[insn & 0xFu], res;
        int wide = (insn & 0x00400000u) == 0;               /* bit 22 clear */
        if ((insn & 0xF0u) == 0x30u) {
            if (wide) {                                     /* REV */
                res = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
                      ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
            } else {                                        /* RBIT */
                uint32_t i;
                res = 0;
                for (i = 0; i < 32; i++)
                    if (v & (1u << i))
                        res |= 1u << (31 - i);
            }
        } else if (wide) {                                  /* REV16 */
            res = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
        } else {                                            /* REVSH */
            res = (uint32_t)(int32_t)(int16_t)
                  (((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
        }
        c->r[rd] = res;
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    /* UADD8 / SEL, the A32 spelling of the pair the Thumb decoder already has:
     * UADD8 sets one GE bit per byte lane, SEL then consumes them. The CRT's
     * SIMD string routines exist in both instruction sets. */
    if ((insn & 0x0FF00FF0u) == 0x06500F90u) {              /* UADD8 */
        uint32_t rn = (insn >> 16) & 0xFu, rd = (insn >> 12) & 0xFu;
        uint32_t a = c->r[rn], b = c->r[insn & 0xFu], res = 0, ge = 0, i;
        for (i = 0; i < 4; i++) {
            uint32_t sum = ((a >> (8 * i)) & 0xFFu) +
                           ((b >> (8 * i)) & 0xFFu);
            res |= (sum & 0xFFu) << (8 * i);
            if (sum >= 0x100u)
                ge |= 1u << i;
        }
        c->r[rd] = res;
        c->cpsr = (c->cpsr & ~CPSR_GE_MASK) | (ge << CPSR_GE_SHIFT);
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    if ((insn & 0x0FF00FF0u) == 0x06800FB0u) {              /* SEL */
        uint32_t rn = (insn >> 16) & 0xFu, rd = (insn >> 12) & 0xFu;
        uint32_t rm = insn & 0xFu;
        uint32_t ge = (c->cpsr & CPSR_GE_MASK) >> CPSR_GE_SHIFT;
        uint32_t res = 0, i;
        for (i = 0; i < 4; i++) {
            uint32_t src = (ge & (1u << i)) ? c->r[rn] : c->r[rm];
            res |= src & (0xFFu << (8 * i));
        }
        c->r[rd] = res;
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    /* SSAT / USAT: clamp a shifted operand into a signed or unsigned field.
     * Note the operand widths are encoded differently -- SSAT stores N-1 and
     * USAT stores N. Q is not modelled; nothing here reads it. */
    if ((insn & 0x0FA00030u) == 0x06A00010u) {
        int usat = (int)((insn >> 22) & 1u);
        uint32_t satimm = (insn >> 16) & 0x1Fu;
        uint32_t rd = (insn >> 12) & 0xFu, rn = insn & 0xFu;
        uint32_t imm5 = (insn >> 7) & 0x1Fu;
        int32_t v;

        if ((insn >> 6) & 1u) {                     /* ASR, where #0 means #32 */
            uint32_t amt = imm5 ? imm5 : 32u;
            v = (amt >= 32u) ? ((int32_t)c->r[rn] >> 31)
                             : ((int32_t)c->r[rn] >> amt);
        } else {
            v = (int32_t)(c->r[rn] << imm5);
        }

        if (usat) {
            int32_t hi = (satimm >= 31u) ? (int32_t)0x7FFFFFFF
                                         : (int32_t)((1u << satimm) - 1u);
            if (v < 0) { v = 0; c->cpsr |= CPSR_Q; }
            else if (v > hi) { v = hi; c->cpsr |= CPSR_Q; }
        } else {
            uint32_t n = satimm + 1u;
            int32_t hi = (n >= 32u) ? (int32_t)0x7FFFFFFF
                                    : (int32_t)((1 << (n - 1)) - 1);
            int32_t lo = (n >= 32u) ? (int32_t)0x80000000
                                    : -(int32_t)(1 << (n - 1));
            if (v < lo) { v = lo; c->cpsr |= CPSR_Q; }
            else if (v > hi) { v = hi; c->cpsr |= CPSR_Q; }
        }
        c->r[rd] = (uint32_t)v;
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    /* BFI / BFC: insert Rn<width-1:0> into Rd<msb:lsb>, leaving the rest of Rd
     * alone. Rn=1111 is BFC, which clears the field instead. */
    if ((insn & 0x0FE00070u) == 0x07C00010u) {
        uint32_t msb = (insn >> 16) & 0x1F, lsb = (insn >> 7) & 0x1F;
        uint32_t rd = (insn >> 12) & 0xF, rn = insn & 0xF;
        if (msb >= lsb) {
            uint32_t width = msb - lsb + 1u;
            uint32_t mask = (width >= 32u) ? 0xFFFFFFFFu
                                           : (((1u << width) - 1u) << lsb);
            uint32_t v = (rn == 15) ? 0u : ((c->r[rn] << lsb) & mask);
            c->r[rd] = (c->r[rd] & ~mask) | v;
        }
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    /* Media instructions (UXTB/SXTH/UBFX/...) live in the load/store encoding
     * space but are distinguished by bit 4 being set in the register-offset
     * form. Without this they decode as loads and silently produce garbage. */
    if ((insn & 0x0F8000F0u) == 0x06800070u) {              /* SXT/UXT(A)B/H */
        uint32_t op = (insn >> 20) & 7;
        uint32_t rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
        uint32_t rot = ((insn >> 10) & 3) * 8;
        uint32_t v = c->r[insn & 0xF];
        uint32_t res;
        v = rot ? ((v >> rot) | (v << (32 - rot))) : v;
        switch (op) {
        case 2: res = (uint32_t)(int32_t)(int8_t)v; break;          /* SXTB */
        case 3: res = (uint32_t)(int32_t)(int16_t)v; break;         /* SXTH */
        case 6: res = v & 0xFFu; break;                             /* UXTB */
        case 7: res = v & 0xFFFFu; break;                           /* UXTH */
        default: UNDEF(g, pc, insn);
        }
        if (rn != 15)
            res += c->r[rn];                       /* the accumulate forms */
        c->r[rd] = res;
        c->r[15] = pc + 4;
        return GUEST_OK;
    }
    if ((insn & 0x0FE00070u) == 0x07A00050u ||              /* SBFX */
        (insn & 0x0FE00070u) == 0x07E00050u) {              /* UBFX */
        uint32_t rd = (insn >> 12) & 0xF, rn = insn & 0xF;
        uint32_t lsb = (insn >> 7) & 0x1F;
        uint32_t width = ((insn >> 16) & 0x1F) + 1u;
        uint32_t v = 0;
        if (lsb + width <= 32) {
            v = (c->r[rn] >> lsb);
            if (width < 32)
                v &= (1u << width) - 1u;
            if ((insn & 0x00400000u) == 0 && width < 32 &&
                (v & (1u << (width - 1))))
                v |= ~((1u << width) - 1u);                 /* SBFX sign */
        }
        c->r[rd] = v;
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

arm_load_store:
    if ((insn & 0x0C000000u) == 0x04000000u &&
        !((insn & 0x02000000u) && (insn & 0x10u))) {        /* LDR/STR (B) */
        uint32_t rn = (insn >> 16) & 0xF, rt = (insn >> 12) & 0xF;
        int P = (int)((insn >> 24) & 1), U = (int)((insn >> 23) & 1);
        int B = (int)((insn >> 22) & 1), W = (int)((insn >> 21) & 1);
        int L = (int)((insn >> 20) & 1);
        uint32_t base = (rn == 15) ? read_pc : c->r[rn];
        uint32_t off;

        if (insn & 0x02000000u) {
            int cout;
            off = shift_c(c->r[insn & 0xF], (int)((insn >> 5) & 3),
                          (int)((insn >> 7) & 0x1F), get_c(c), &cout);
        } else {
            off = insn & 0xFFF;
        }

        uint32_t addr = P ? (U ? base + off : base - off) : base;
        uint32_t wb = U ? base + off : base - off;

        if (L) {
            uint32_t v;
            if (B ? !guest_ld8(&g->mem, addr, &v)
                  : !guest_ld32(&g->mem, addr, &v))
                MEMFAULT(g, addr);
            if ((!P || W) && rn != 15)
                c->r[rn] = wb;
            if (rt == 15) {
                branch_interworking(c, v);
                return GUEST_OK;
            }
            c->r[rt] = v;
        } else {
            uint32_t v = (rt == 15) ? read_pc : c->r[rt];
            if (B ? !guest_st8(&g->mem, addr, v)
                  : !guest_st32(&g->mem, addr, v))
                MEMFAULT(g, addr);
            if ((!P || W) && rn != 15)
                c->r[rn] = wb;
        }
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    if ((insn & 0x0E000000u) == 0x08000000u) {              /* LDM / STM */
        uint32_t rn = (insn >> 16) & 0xF, list = insn & 0xFFFF;
        int P = (int)((insn >> 24) & 1), U = (int)((insn >> 23) & 1);
        int W = (int)((insn >> 21) & 1), L = (int)((insn >> 20) & 1);
        int n = 0;
        for (int i = 0; i < 16; i++)
            if (list & (1u << i))
                n++;

        uint32_t base = c->r[rn];
        uint32_t addr = U ? (base + (P ? 4u : 0u)) : (base - 4u * n + (P ? 0u : 4u));
        uint32_t new_base = U ? base + 4u * n : base - 4u * n;
        uint32_t branch_to = 0;
        int branched = 0;

        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i)))
                continue;
            if (L) {
                uint32_t v;
                if (!guest_ld32(&g->mem, addr, &v))
                    MEMFAULT(g, addr);
                if (i == 15) {
                    branch_to = v;
                    branched = 1;
                } else {
                    c->r[i] = v;
                }
            } else {
                uint32_t v = (i == 15) ? read_pc : c->r[i];
                if (!guest_st32(&g->mem, addr, v))
                    MEMFAULT(g, addr);
            }
            addr += 4;
        }
        if (W)
            c->r[rn] = new_base;
        if (branched) {
            branch_interworking(c, branch_to);
            return GUEST_OK;
        }
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    /* MOVW / MOVT must be tested before data processing: their bits 24..21
     * decode as ADD and CMP respectively, so a general dispatch silently
     * computes garbage (and MOVT, reading as a compare, writes nothing). */
    if ((insn & 0x0FF00000u) == 0x03000000u) {              /* MOVW */
        uint32_t rd = (insn >> 12) & 0xF;
        c->r[rd] = ((insn >> 4) & 0xF000u) | (insn & 0xFFFu);
        c->r[15] = pc + 4;
        return GUEST_OK;
    }
    if ((insn & 0x0FF00000u) == 0x03400000u) {              /* MOVT */
        uint32_t rd = (insn >> 12) & 0xF;
        c->r[rd] = (c->r[rd] & 0xFFFFu) |
                   ((((insn >> 4) & 0xF000u) | (insn & 0xFFFu)) << 16);
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    /* Extension space (bits 27..25 = 000, bits 7..4 = 1xx1): multiply and the
     * halfword/signed loads. These also fall into the data-processing mask and
     * would be mis-decoded as register-shifted operations. */
    if ((insn & 0x0E000000u) == 0 && (insn & 0x90u) == 0x90u) {
        uint32_t rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
        uint32_t rs = (insn >> 8) & 0xF, rm = insn & 0xF;

        if ((insn & 0xF0u) == 0x90u) {                      /* multiply */
            uint32_t op = (insn >> 21) & 0xF, S = (insn >> 20) & 1;
            if (op == 0 || op == 1) {                       /* MUL / MLA */
                uint32_t res = c->r[rm] * c->r[rs];
                if (op == 1)
                    res += c->r[rd];
                c->r[rn] = res;                    /* Rd and Rn swap here */
                if (S)
                    set_nz(c, res);
                c->r[15] = pc + 4;
                return GUEST_OK;
            }
            /* MLS: Rd = Ra - Rn*Rm. Same field swap as MUL/MLA -- the
             * destination is bits 19:16 and the accumulate operand is
             * bits 15:12. The Thumb decoder already had this one. */
            if (op == 3) {
                c->r[rn] = c->r[rd] - c->r[rm] * c->r[rs];
                c->r[15] = pc + 4;
                return GUEST_OK;
            }
            if (op >= 4 && op <= 7) {                       /* UMULL/SMULL.. */
                uint64_t p;
                if (op & 2)
                    p = (uint64_t)((int64_t)(int32_t)c->r[rm] *
                                   (int64_t)(int32_t)c->r[rs]);
                else
                    p = (uint64_t)c->r[rm] * (uint64_t)c->r[rs];
                if (op & 1)
                    p += ((uint64_t)c->r[rn] << 32) | c->r[rd];
                c->r[rd] = (uint32_t)p;
                c->r[rn] = (uint32_t)(p >> 32);
                if (S)
                    set_nz(c, (uint32_t)(p >> 32));
                c->r[15] = pc + 4;
                return GUEST_OK;
            }
            UNDEF(g, pc, insn);
        }

        {                                    /* halfword / signed load-store */
            int P = (int)((insn >> 24) & 1), U = (int)((insn >> 23) & 1);
            int I = (int)((insn >> 22) & 1), W = (int)((insn >> 21) & 1);
            int L = (int)((insn >> 20) & 1);
            uint32_t off = I ? (((insn >> 4) & 0xF0u) | (insn & 0xFu))
                             : c->r[rm];
            uint32_t base = (rn == 15) ? pc + 8 : c->r[rn];
            uint32_t wb = U ? base + off : base - off;
            uint32_t addr = P ? wb : base;
            uint32_t v;
            uint32_t kind = (insn >> 5) & 3;   /* 1=H 2=SB 3=SH */

            /* L=0 does not mean "store halfword" across this whole space:
             * bits 7..4 = 1101 is LDRD and 1111 is STRD, both 64-bit, and Rt2
             * is implicitly Rt+1. Running them as STRH moved two bytes where
             * eight were meant, so a 64-bit helper's result buffer kept its
             * previous contents and the caller popped stale stack. */
            if (!L && kind >= 2) {
                uint32_t rd2 = (rd + 1u) & 0xFu;
                if (kind == 2) {                            /* LDRD */
                    uint32_t v2;
                    if (!guest_ld32(&g->mem, addr, &v) ||
                        !guest_ld32(&g->mem, addr + 4u, &v2))
                        MEMFAULT(g, addr);
                    c->r[rd] = v;
                    c->r[rd2] = v2;
                } else if (!guest_st32(&g->mem, addr, c->r[rd]) ||
                           !guest_st32(&g->mem, addr + 4u, c->r[rd2])) {
                    MEMFAULT(g, addr);                      /* STRD */
                }
            } else if (L) {
                if (kind == 2) {
                    if (!guest_ld8(&g->mem, addr, &v))
                        MEMFAULT(g, addr);
                    v = (uint32_t)(int32_t)(int8_t)v;
                } else {
                    if (!guest_ld16(&g->mem, addr, &v))
                        MEMFAULT(g, addr);
                    if (kind == 3)
                        v = (uint32_t)(int32_t)(int16_t)v;
                }
                c->r[rd] = v;
            } else if (!guest_st16(&g->mem, addr, c->r[rd])) {
                MEMFAULT(g, addr);
            }
            if ((!P || W) && rn != 15)
                c->r[rn] = wb;
            c->r[15] = pc + 4;
            return GUEST_OK;
        }
    }

    /* ARMv5TE halfword multiplies: SMUL<x><y>, SMLA<x><y>, SMLAL<x><y>,
     * SMULW<y>, SMLAW<y>. They live in the miscellaneous space (bit 7 set,
     * bit 4 clear), so without this the guard below stops on them. x and y
     * pick the bottom or top halfword of Rn and Rm respectively. */
    if ((insn & 0x0F900090u) == 0x01000080u) {
        uint32_t op = (insn >> 21) & 3u;
        uint32_t rd = (insn >> 16) & 0xFu, ra = (insn >> 12) & 0xFu;
        uint32_t rm = (insn >> 8) & 0xFu, rn = insn & 0xFu;
        uint32_t xsel = (insn >> 5) & 1u, ysel = (insn >> 6) & 1u;
        int32_t nh = (int32_t)(int16_t)(xsel ? (c->r[rn] >> 16) : c->r[rn]);
        int32_t mh = (int32_t)(int16_t)(ysel ? (c->r[rm] >> 16) : c->r[rm]);

        if (op == 0) {                                  /* SMLA<x><y> */
            c->r[rd] = c->r[ra] + (uint32_t)(nh * mh);
        } else if (op == 3) {                           /* SMUL<x><y> */
            c->r[rd] = (uint32_t)(nh * mh);
        } else if (op == 1) {                           /* SMLAW / SMULW */
            int64_t p = (int64_t)(int32_t)c->r[rn] * (int64_t)mh;
            uint32_t v = (uint32_t)(uint64_t)(p >> 16);
            c->r[rd] = xsel ? v : v + c->r[ra];         /* x picks which */
        } else {                                        /* SMLAL<x><y> */
            int64_t acc = (int64_t)(((uint64_t)c->r[rd] << 32) | c->r[ra]);
            acc += (int64_t)nh * (int64_t)mh;
            c->r[ra] = (uint32_t)(uint64_t)acc;
            c->r[rd] = (uint32_t)(((uint64_t)acc) >> 32);
        }
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

arm_data_processing:
    if ((insn & 0x0C000000u) == 0x00000000u) {              /* data processing */
        uint32_t op = (insn >> 21) & 0xF, S = (insn >> 20) & 1;
        uint32_t rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;

        /* S==0 with op in 0b10xx is not data processing: that is the
         * miscellaneous space (BX/BLX/CLZ/MSR/MRS/BKPT/saturating adds), and
         * the handled ones are matched above. Falling through runs them as
         * TST/TEQ/CMP/CMN, which write nothing and quietly rewrite the flags --
         * `clz r3,r0` came out as `cmn` and cleared C with no result. UNDEF
         * here so the next one announces itself instead. */
        if (!S && (op & 0xCu) == 0x8u)
            UNDEF(g, pc, insn);
        int cout = get_c(c);
        uint32_t a = (rn == 15) ? read_pc : c->r[rn];
        uint32_t b;

        if (insn & 0x02000000u) {
            b = arm_imm(insn, get_c(c), &cout);
        } else if ((insn & 0x10u) == 0) {
            uint32_t rm = ((insn & 0xF) == 15) ? read_pc : c->r[insn & 0xF];
            b = shift_c(rm, (int)((insn >> 5) & 3), (int)((insn >> 7) & 0x1F),
                        get_c(c), &cout);
        } else {
            uint32_t amt = c->r[(insn >> 8) & 0xF] & 0xFF;
            b = shift_c(c->r[insn & 0xF], (int)((insn >> 5) & 3), (int)amt,
                        get_c(c), &cout);
        }

        uint32_t res = 0;
        int logical = 0, writes = 1;
        switch (op) {
        case 0x0: res = a & b;  logical = 1; break;
        case 0x1: res = a ^ b;  logical = 1; break;
        case 0x2: res = add_with_carry(c, a, ~b, 1, (int)S); break;
        case 0x3: res = add_with_carry(c, ~a, b, 1, (int)S); break;
        case 0x4: res = add_with_carry(c, a, b, 0, (int)S); break;
        case 0x5: res = add_with_carry(c, a, b, get_c(c), (int)S); break;
        case 0x6: res = add_with_carry(c, a, ~b, get_c(c), (int)S); break;
        case 0x7: res = add_with_carry(c, ~a, b, get_c(c), (int)S); break;
        case 0x8: res = a & b;  logical = 1; writes = 0; break;
        case 0x9: res = a ^ b;  logical = 1; writes = 0; break;
        case 0xA: res = add_with_carry(c, a, ~b, 1, 1); writes = 0; break;
        case 0xB: res = add_with_carry(c, a, b, 0, 1);  writes = 0; break;
        case 0xC: res = a | b;  logical = 1; break;
        case 0xD: res = b;      logical = 1; break;
        case 0xE: res = a & ~b; logical = 1; break;
        default:  res = ~b;     logical = 1; break;
        }
        if (logical && (S || !writes)) {
            set_nz(c, res);
            set_c(c, cout);
        }
        if (writes) {
            if (rd == 15) {
                branch_interworking(c, res);
                return GUEST_OK;
            }
            c->r[rd] = res;
        }
        c->r[15] = pc + 4;
        return GUEST_OK;
    }

    UNDEF(g, pc, insn);
}

/* ------------------------------------------------------------------- VFP ---
 * Single- and double-precision VFPv3. The ABI is softfp, so no float ever
 * crosses a function boundary in s0-s15, but all arithmetic runs here: 15.7%
 * of 32-bit Thumb-2 instructions in this image are VFP.
 *
 * s[0..31] holds raw bits; d<n> aliases s[2n] (low) and s[2n+1] (high).
 */

static float vs_get(const GuestCpu *c, uint32_t n) {
    float f;
    memcpy(&f, &c->s[n & 31], 4);
    return f;
}

static void vs_set(GuestCpu *c, uint32_t n, float f) {
    memcpy(&c->s[n & 31], &f, 4);
}

static double vd_get(const GuestCpu *c, uint32_t n) {
    uint64_t u = (uint64_t)c->s[(2 * n) & 63] |
                 ((uint64_t)c->s[(2 * n + 1) & 63] << 32);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static void vd_set(GuestCpu *c, uint32_t n, double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    c->s[(2 * n) & 63] = (uint32_t)u;
    c->s[(2 * n + 1) & 63] = (uint32_t)(u >> 32);
}

/* VFPExpandImm: a : NOT(b) : b*5 : cdefgh : 0*19 */
static uint32_t vfp_expand_imm(uint32_t imm8) {
    uint32_t a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, rest = imm8 & 0x3F;
    return (a << 31) | ((b ? 0u : 1u) << 30) | ((b ? 0x1Fu : 0u) << 25) |
           (rest << 19);
}

/* VCMP writes NZCV into FPSCR<31:28>; VMRS later copies them to CPSR. */
static void vfp_cmp(GuestCpu *c, double a, double b, int unordered_is_nan) {
    uint32_t nzcv;
    (void)unordered_is_nan;
    if (a != a || b != b)
        nzcv = 0x3u;                       /* unordered: C=1 V=1 */
    else if (a == b)
        nzcv = 0x6u;                       /* Z=1 C=1 */
    else if (a < b)
        nzcv = 0x8u;                       /* N=1 */
    else
        nzcv = 0x2u;                       /* C=1 */
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (nzcv << 28);
}

static GuestStatus step_vfp(Guest *g, uint32_t pc, uint32_t hw, uint32_t hw2,
                           int arm) {
    GuestCpu *c = &g->cpu;
    const uint32_t next = pc + 4;
    const uint32_t insn = (hw << 16) | hw2;
    /* A literal load reads PC+8 in ARM but Align(PC+4,4) in Thumb; the
     * rest of the encoding is identical between the two. */
    const uint32_t lit_pc = arm ? (pc + 8u) : ((pc + 4u) & ~3u);

    uint32_t D = (hw >> 6) & 1, Vd = (hw2 >> 12) & 0xF;
    uint32_t Vn = hw & 0xF, N = (hw2 >> 7) & 1;
    uint32_t Vm = hw2 & 0xF, M = (hw2 >> 5) & 1;
    int sz = (int)((hw2 >> 8) & 1);          /* 0 = f32, 1 = f64 */
    uint32_t d = sz ? ((D << 4) | Vd) : ((Vd << 1) | D);
    uint32_t n = sz ? ((N << 4) | Vn) : ((Vn << 1) | N);
    uint32_t m = sz ? ((M << 4) | Vm) : ((Vm << 1) | M);

    /* ---- VMRS / VMSR: 1110 1110 111x 0001 Rt 1010 0001 0000 ------------ */
    if ((hw & 0xFFE0u) == 0xEEE0u && (hw2 & 0x0F1Fu) == 0x0A10u) {
        uint32_t rt = (hw2 >> 12) & 0xF;
        if (hw & 0x0010u) {                                     /* VMRS */
            if (rt == 15)                    /* VMRS APSR_nzcv, FPSCR */
                c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (c->fpscr & 0xF0000000u);
            else
                c->r[rt] = c->fpscr;
        } else {                                                /* VMSR */
            c->fpscr = c->r[rt];
        }
        c->r[15] = next;
        return GUEST_OK;
    }

    /* ---- VMOV core <-> single: 1110 1110 000o Vn Rt 1010 N001 0000 ----- */
    if ((hw & 0xFFE0u) == 0xEE00u && (hw2 & 0x0F7Fu) == 0x0A10u) {
        uint32_t rt = (hw2 >> 12) & 0xF;
        uint32_t sn = (Vn << 1) | N;
        if (hw & 0x0010u)
            c->r[rt] = c->s[sn & 31];                           /* VMOV Rt, Sn */
        else
            c->s[sn & 31] = c->r[rt];                           /* VMOV Sn, Rt */
        c->r[15] = next;
        return GUEST_OK;
    }

    /* ---- VMOV between two core registers and a 64-bit extension register,
     * or a pair of consecutive singles: 1100 010L Rt2 Rt 101x 00M1 Vm.
     * This MUST be tested before the block below: it shares the 110x
     * coprocessor prefix, so otherwise it decodes as VLDM/VSTM and executes
     * as a memory access using Rt2 as the base -- `vmov r0,r1,d9` with r1=0
     * faulted at 0xffffff9c despite touching no memory at all. */
    if ((hw & 0x0FE0u) == 0x0C40u && (hw2 & 0x0E10u) == 0x0A10u) {
        uint32_t rt = (hw2 >> 12) & 0xFu, rt2 = hw & 0xFu;
        int to_core = (int)((hw >> 4) & 1u);       /* insn bit 20 */
        uint32_t lo = sz ? (2u * m) : m;           /* d(m) is s[2m]:s[2m+1] */
        uint32_t hi = lo + 1u;

        if (to_core) {
            c->r[rt] = c->s[lo & 63u];
            c->r[rt2] = c->s[hi & 63u];
        } else {
            c->s[lo & 63u] = c->r[rt];
            c->s[hi & 63u] = c->r[rt2];
        }
        c->r[15] = next;
        return GUEST_OK;
    }

    /* ---- VLDR / VSTR / VLDM / VSTM: 1110 110P UDWL Rn ------------------- */
    if ((hw & 0xFE00u) == 0xEC00u) {
        uint32_t rn = hw & 0xF;
        int P = (int)((hw >> 8) & 1), U = (int)((hw >> 7) & 1);
        int W = (int)((hw >> 5) & 1), L = (int)((hw >> 4) & 1);
        uint32_t imm8 = hw2 & 0xFFu;
        uint32_t base = (rn == 15) ? lit_pc : c->r[rn];

        if (P && !W) {                                          /* VLDR/VSTR */
            uint32_t addr = U ? base + imm8 * 4u : base - imm8 * 4u;
            if (sz) {
                uint32_t lo, hi;
                if (L) {
                    if (!guest_ld32(&g->mem, addr, &lo) ||
                        !guest_ld32(&g->mem, addr + 4, &hi))
                        MEMFAULT(g, addr);
                    c->s[(2 * d) & 63] = lo;
                    c->s[(2 * d + 1) & 63] = hi;
                } else {
                    if (!guest_st32(&g->mem, addr, c->s[(2 * d) & 63]) ||
                        !guest_st32(&g->mem, addr + 4, c->s[(2 * d + 1) & 63]))
                        MEMFAULT(g, addr);
                }
            } else {
                uint32_t v;
                if (L) {
                    if (!guest_ld32(&g->mem, addr, &v))
                        MEMFAULT(g, addr);
                    c->s[d & 31] = v;
                } else if (!guest_st32(&g->mem, addr, c->s[d & 31])) {
                    MEMFAULT(g, addr);
                }
            }
            c->r[15] = next;
            return GUEST_OK;
        }

        {                                     /* VLDM / VSTM / VPUSH / VPOP */
            uint32_t count = sz ? (imm8 / 2u) : imm8;
            uint32_t addr = U ? base : base - imm8 * 4u;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t reg = d + i;
                if (sz) {
                    uint32_t lo, hi;
                    if (L) {
                        if (!guest_ld32(&g->mem, addr, &lo) ||
                            !guest_ld32(&g->mem, addr + 4, &hi))
                            MEMFAULT(g, addr);
                        c->s[(2 * reg) & 63] = lo;
                        c->s[(2 * reg + 1) & 63] = hi;
                    } else if (!guest_st32(&g->mem, addr, c->s[(2 * reg) & 63]) ||
                               !guest_st32(&g->mem, addr + 4,
                                           c->s[(2 * reg + 1) & 63])) {
                        MEMFAULT(g, addr);
                    }
                    addr += 8;
                } else {
                    uint32_t v;
                    if (L) {
                        if (!guest_ld32(&g->mem, addr, &v))
                            MEMFAULT(g, addr);
                        c->s[reg & 31] = v;
                    } else if (!guest_st32(&g->mem, addr, c->s[reg & 31])) {
                        MEMFAULT(g, addr);
                    }
                    addr += 4;
                }
            }
            if (W)
                c->r[rn] = U ? base + imm8 * 4u : base - imm8 * 4u;
            c->r[15] = next;
            return GUEST_OK;
        }
    }

    /* ---- data processing: 1110 1110 o1 D o2 Vn Vd 101 sz N o3 M 0 Vm --- */
    if ((hw & 0xFF00u) == 0xEE00u) {
        uint32_t o1 = (hw >> 7) & 1, o2 = (hw >> 4) & 3;
        uint32_t o3 = (hw2 >> 6) & 1;
        /* opc3 is bits 7:6. Every form in the "other" group below except VMOV
         * (immediate) has bit 6 SET, so bit 6 cannot tell VNEG from VSQRT,
         * VMOV(reg) from VABS, or unsigned from signed VCVT -- opc3<1>, bit 7,
         * is the discriminator. Using o3 there made `vneg` run as `vsqrt`. */
        uint32_t opc3hi = (hw2 >> 7) & 1;

        if (!o1) {
            /* VFPv3 VMLA/VMLS/VNMLA/VNMLS are NOT fused: the product rounds to
             * the operand precision, then the accumulate rounds again. Doing
             * the whole thing in double and rounding once is a fused multiply-
             * add and differs in the last bit, so single precision has to be
             * computed in float, with the product forced into its own
             * rounding. The negations follow the spec's form (FPAdd of negated
             * operands) rather than an algebraic rearrangement, which would
             * get signed zeros wrong. */
            if (sz) {
                double a = vd_get(c, n), b = vd_get(c, m), acc = vd_get(c, d);
                double p, r;
                switch (o2) {
                case 0: p = a * b; r = o3 ? acc + (-p) : acc + p; break;
                case 1: p = a * b; r = o3 ? (-acc) + (-p) : (-acc) + p; break;
                case 2: r = o3 ? -(a * b) : a * b; break;
                default: r = o3 ? a - b : a + b; break;
                }
                vd_set(c, d, r);
            } else {
                float a = vs_get(c, n), b = vs_get(c, m), acc = vs_get(c, d);
                float p, r;
                switch (o2) {
                case 0: p = a * b; r = o3 ? acc + (-p) : acc + p; break;
                case 1: p = a * b; r = o3 ? (-acc) + (-p) : (-acc) + p; break;
                case 2: r = o3 ? -(a * b) : a * b; break;
                default: r = o3 ? a - b : a + b; break;
                }
                vs_set(c, d, r);
            }
            c->r[15] = next;
            return GUEST_OK;
        }

        if (o2 == 0 && !o3) {                                    /* VDIV */
            if (sz)
                vd_set(c, d, vd_get(c, n) / vd_get(c, m));
            else
                vs_set(c, d, vs_get(c, n) / vs_get(c, m));
            c->r[15] = next;
            return GUEST_OK;
        }

        /* VMOV (immediate) must be pulled out before the switch below: in this
         * encoding bits 19..16 are imm4H, NOT a sub-opcode, so dispatching on
         * Vn only matched while the immediate's high nibble happened to be
         * zero. `vmov.f64 d7,#0x60` has imm4H=6 and fell through to UNDEF.
         * It is the only form here with opc3<0> clear; every other one
         * (VABS/VNEG/VSQRT/VCMP/VCVT) has it set. */
        if (o2 == 3 && !o3) {
            uint32_t imm8 = ((hw & 0xFu) << 4) | (hw2 & 0xFu);
            if (sz) {
                /* VFPExpandImm for N=64, built directly. Deriving it from the
                 * 32-bit expansion drops imm8<5:4> -- the low two bits of the
                 * 11-bit exponent -- because the single-precision exponent is
                 * only 8 bits wide. That turned `vmov.f64 d7,#1.0` (imm8=0x70,
                 * exponent 0x3FF) into exponent 0x3FC, i.e. 0.125. #2.0 has
                 * those bits clear, which is why it looked correct.
                 *   exp = NOT(b6) : Replicate(b6,8) : imm8<5:4>
                 *   frac = imm8<3:0> : Zeros(48) */
                uint32_t b6 = (imm8 >> 6) & 1u;
                uint32_t exp = ((b6 ^ 1u) << 10) | (b6 ? 0x3FCu : 0u) |
                               ((imm8 >> 4) & 3u);
                c->s[(2 * d) & 63] = 0;
                c->s[(2 * d + 1) & 63] = (((imm8 >> 7) & 1u) << 31) |
                                         (exp << 20) |
                                         ((imm8 & 0xFu) << 16);
            } else {
                c->s[d & 31] = vfp_expand_imm(imm8);
            }
            c->r[15] = next;
            return GUEST_OK;
        }

        if (o2 == 3) {                        /* the "other" group, by Vn */
            switch (Vn) {
            case 0x0:
                if (opc3hi) {                                    /* VABS */
                    if (sz)
                        vd_set(c, d, fabs(vd_get(c, m)));
                    else
                        vs_set(c, d, fabsf(vs_get(c, m)));
                } else {                                         /* VMOV reg */
                    if (sz)
                        vd_set(c, d, vd_get(c, m));
                    else
                        vs_set(c, d, vs_get(c, m));
                }
                break;
            case 0x1:
                if (opc3hi) {                                    /* VSQRT */
                    if (sz)
                        vd_set(c, d, sqrt(vd_get(c, m)));
                    else
                        vs_set(c, d, sqrtf(vs_get(c, m)));
                } else {                                         /* VNEG */
                    if (sz)
                        vd_set(c, d, -vd_get(c, m));
                    else
                        vs_set(c, d, -vs_get(c, m));
                }
                break;
            case 0x4:                                            /* VCMP(E) */
                vfp_cmp(c, sz ? vd_get(c, d) : (double)vs_get(c, d),
                        sz ? vd_get(c, m) : (double)vs_get(c, m), (int)opc3hi);
                break;
            case 0x5:                                            /* VCMP(E) #0 */
                vfp_cmp(c, sz ? vd_get(c, d) : (double)vs_get(c, d), 0.0,
                        (int)opc3hi);
                break;
            case 0x7:                                     /* VCVT f32 <-> f64 */
                if (sz)
                    vs_set(c, (Vd << 1) | D, (float)vd_get(c, m));
                else
                    vd_set(c, (D << 4) | Vd, (double)vs_get(c, (Vm << 1) | M));
                break;
            case 0x8: {                                   /* VCVT int -> f */
                uint32_t sm = (Vm << 1) | M;
                /* op (bit 7) selects signed; bit 6 is set either way. */
                double v = opc3hi ? (double)(int32_t)c->s[sm & 31]
                                  : (double)c->s[sm & 31];
                if (sz)
                    vd_set(c, d, v);
                else
                    vs_set(c, d, (float)v);
                break;
            }
            /* VCVT between floating-point and fixed-point. opc2 = 1 op 1 U:
             * op (bit 18) picks the direction, U (bit 16) the signedness;
             * sx (hw2 bit 7) picks a 16- or 32-bit fixed format and the
             * fraction width is size - (imm4:i). Source and destination are
             * the same register. */
            case 0xA: case 0xB: case 0xE: case 0xF: {
                int to_fixed = (int)((Vn >> 2) & 1u);
                int uns = (int)(Vn & 1u);
                int sx = (int)((hw2 >> 7) & 1u);
                uint32_t imm = ((hw2 & 0xFu) << 1) | ((hw2 >> 5) & 1u);
                double scale = ldexp(1.0, (sx ? 32 : 16) - (int)imm);
                uint32_t raw = sz ? c->s[(2 * d) & 63] : c->s[d & 31];
                double v;

                if (to_fixed) {                 /* float -> fixed, toward zero */
                    double lim;
                    v = (sz ? vd_get(c, d) : (double)vs_get(c, d)) * scale;
                    v = (v < 0) ? ceil(v) : floor(v);
                    lim = sx ? 4294967296.0 : 65536.0;
                    if (uns) {
                        if (v < 0) v = 0;
                        if (v > lim - 1) v = lim - 1;
                        raw = (uint32_t)v;
                    } else {
                        if (v < -(lim / 2)) v = -(lim / 2);
                        if (v > lim / 2 - 1) v = lim / 2 - 1;
                        raw = (uint32_t)(int32_t)v;
                    }
                    if (!sx)
                        raw &= 0xFFFFu;
                    if (sz)
                        c->s[(2 * d) & 63] = raw;
                    else
                        c->s[d & 31] = raw;
                } else {                        /* fixed -> float */
                    if (sx)
                        v = uns ? (double)raw : (double)(int32_t)raw;
                    else
                        v = uns ? (double)(uint16_t)raw
                                : (double)(int16_t)raw;
                    v /= scale;
                    if (sz)
                        vd_set(c, d, v);
                    else
                        vs_set(c, d, (float)v);
                }
                break;
            }

            case 0xC:
            case 0xD: {                                   /* VCVT/VCVTR f->int */
                double v = sz ? vd_get(c, m) : (double)vs_get(c, (Vm << 1) | M);
                uint32_t sd = (Vd << 1) | D;
                /* op (bit 7): 1 = round toward zero (VCVT); 0 = use the FPSCR
                 * rounding mode (VCVTR), i.e. round-to-nearest-even here. */
                if (!opc3hi)
                    v = rint(v);
                if (Vn & 1)
                    c->s[sd & 31] = (uint32_t)(int32_t)v;        /* signed */
                else
                    c->s[sd & 31] = (uint32_t)(v < 0 ? 0 : v);   /* unsigned */
                break;
            }
            default:
                UNDEF(g, pc, insn);
            }
            c->r[15] = next;
            return GUEST_OK;
        }
        UNDEF(g, pc, insn);
    }

    UNDEF(g, pc, insn);
}

/* ------------------------------------------------------- Thumb-2 (32-bit) */

/* ThumbExpandImm_C: the 12-bit modified immediate. Either a byte replicated
 * into one of four patterns, or an 8-bit value with an implicit top bit
 * rotated right -- the carry-out only comes from the rotated form. */
static uint32_t thumb_expand_imm(uint32_t imm12, int cin, int *cout) {
    uint32_t b = imm12 & 0xFF;
    *cout = cin;
    if ((imm12 & 0xC00u) == 0) {
        switch ((imm12 >> 8) & 3) {
        case 0: return b;
        case 1: return (b << 16) | b;
        case 2: return (b << 24) | (b << 8);
        default: return (b << 24) | (b << 16) | (b << 8) | b;
        }
    }
    {
        uint32_t unrot = 0x80u | (imm12 & 0x7Fu);
        int rot = (int)((imm12 >> 7) & 0x1F);
        uint32_t v = (unrot >> rot) | (unrot << (32 - rot));
        *cout = (int)(v >> 31);
        return v;
    }
}

/* Shared by the modified-immediate and shifted-register data-processing
 * groups; `writes` comes back 0 for the compare-only forms. */
static uint32_t dp_compute(GuestCpu *c, uint32_t op, uint32_t a, uint32_t b,
                           int setflags, int cout, int *writes) {
    uint32_t res = 0;
    int logical = 0;
    *writes = 1;
    switch (op) {
    case 0x0: res = a & b;  logical = 1; break;                      /* AND */
    case 0x1: res = a & ~b; logical = 1; break;                      /* BIC */
    case 0x2: res = a | b;  logical = 1; break;                      /* ORR/MOV */
    case 0x3: res = a | ~b; logical = 1; break;                      /* ORN/MVN */
    case 0x4: res = a ^ b;  logical = 1; break;                      /* EOR */
    case 0x8: res = add_with_carry(c, a, b, 0, setflags); break;     /* ADD */
    case 0xA: res = add_with_carry(c, a, b, get_c(c), setflags); break;  /* ADC */
    case 0xB: res = add_with_carry(c, a, ~b, get_c(c), setflags); break; /* SBC */
    case 0xD: res = add_with_carry(c, a, ~b, 1, setflags); break;    /* SUB */
    case 0xE: res = add_with_carry(c, ~a, b, 1, setflags); break;    /* RSB */
    default: *writes = -1; return 0;                                 /* unhandled */
    }
    if (logical && setflags) {
        set_nz(c, res);
        set_c(c, cout);
    }
    return res;
}

static GuestStatus step_thumb32(Guest *g, uint32_t pc, uint32_t hw, uint32_t hw2) {
    GuestCpu *c = &g->cpu;
    const uint32_t read_pc = pc + 4;
    const uint32_t lit_pc = (pc + 4) & ~3u;
    const uint32_t next = pc + 4;
    const uint32_t insn = (hw << 16) | hw2;

    if ((hw & 0xFC00u) == 0xEC00u)
        return step_vfp(g, pc, hw, hw2, 0);

    /* ---- branches and misc control: hw1 = 11110, hw2 bit15 = 1 ---------- */
    if ((hw & 0xF800u) == 0xF000u && (hw2 & 0x8000u)) {
        uint32_t s = (hw >> 10) & 1;
        uint32_t j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;

        if ((hw2 & 0xC000u) == 0xC000u) {
            /* bit12 selects BL (stay in Thumb) from BLX (switch to ARM). The
             * two also differ in the immediate: BLX uses imm10L<10:1> scaled
             * by 4 and branches relative to Align(PC,4), not PC. Treating BLX
             * as BL leaves CPSR.T set and decodes the ARM callee as Thumb. */
            uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
            uint32_t hi = (s << 24) | (i1 << 23) | (i2 << 22) |
                          ((hw & 0x3FFu) << 12);
            c->r[14] = (pc + 4) | 1u;
            if (hw2 & 0x1000u) {                            /* BL */
                int32_t off = (int32_t)((hi | ((hw2 & 0x7FFu) << 1)) << 7) >> 7;
                c->r[15] = read_pc + (uint32_t)off;
            } else {                                        /* BLX */
                int32_t off = (int32_t)((hi | ((hw2 & 0x7FEu) << 1)) << 7) >> 7;
                c->cpsr &= ~CPSR_T;
                c->r[15] = (lit_pc + (uint32_t)off) & ~3u;
            }
            return GUEST_OK;
        }
        if ((hw2 & 0x1000u) == 0) {                        /* B<cond>.W (T3) */
            uint32_t cond = (hw >> 6) & 0xF;
            if (cond < 0xE) {
                if (cond_ok(c->cpsr, cond)) {
                    uint32_t raw = (s << 20) | (j2 << 19) | (j1 << 18) |
                                   ((hw & 0x3Fu) << 12) | ((hw2 & 0x7FFu) << 1);
                    int32_t off = (int32_t)(raw << 11) >> 11;
                    c->r[15] = read_pc + (uint32_t)off;
                    return GUEST_OK;
                }
                c->r[15] = next;
                return GUEST_OK;
            }
            UNDEF(g, pc, insn);            /* MSR/MRS/hints: not needed yet */
        }
        {                                                   /* B.W (T4) */
            uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
            uint32_t raw = (s << 24) | (i1 << 23) | (i2 << 22) |
                           ((hw & 0x3FFu) << 12) | ((hw2 & 0x7FFu) << 1);
            int32_t off = (int32_t)(raw << 7) >> 7;
            c->r[15] = read_pc + (uint32_t)off;
            return GUEST_OK;
        }
    }

    /* ---- data processing, modified immediate: hw1 11110, bit9=0 --------- */
    if ((hw & 0xFA00u) == 0xF000u && !(hw2 & 0x8000u)) {
        uint32_t op = (hw >> 5) & 0xF, S = (hw >> 4) & 1;
        uint32_t rn = hw & 0xF, rd = (hw2 >> 8) & 0xF;
        uint32_t imm12 = (((hw >> 10) & 1) << 11) | (((hw2 >> 12) & 7) << 8) |
                         (hw2 & 0xFF);
        int cout = get_c(c);
        uint32_t b = thumb_expand_imm(imm12, get_c(c), &cout);
        int writes;
        /* T32 has no MOV/MVN opcode of its own: Rn=1111 is what selects them
         * out of ORR/ORN, and no T32 data-processing form reads PC as an
         * operand. Passing c->r[15] here makes `mov.w rd,#imm` compute
         * PC|imm, which is a plausible-looking address, not a visible fault. */
        uint32_t res = dp_compute(c, op, rn == 15 ? 0u : c->r[rn], b,
                                  (int)S || rd == 15, cout, &writes);
        if (writes < 0)
            UNDEF(g, pc, insn);
        if (writes && rd != 15)
            c->r[rd] = res;
        c->r[15] = next;
        return GUEST_OK;
    }

    /* ---- plain binary immediate: hw1 11110, bit9=1 ---------------------- */
    if ((hw & 0xFA00u) == 0xF200u && !(hw2 & 0x8000u)) {
        uint32_t rn = hw & 0xF, rd = (hw2 >> 8) & 0xF;
        uint32_t imm12 = (((hw >> 10) & 1) << 11) | (((hw2 >> 12) & 7) << 8) |
                         (hw2 & 0xFF);
        switch (hw & 0xFBF0u) {
        case 0xF200u:                                   /* ADDW / ADR */
            c->r[rd] = (rn == 15 ? lit_pc : c->r[rn]) + imm12;
            c->r[15] = next;
            return GUEST_OK;
        case 0xF2A0u:                                   /* SUBW / ADR */
            c->r[rd] = (rn == 15 ? lit_pc : c->r[rn]) - imm12;
            c->r[15] = next;
            return GUEST_OK;
        case 0xF240u:                                   /* MOVW */
            c->r[rd] = (rn << 12) | imm12;
            c->r[15] = next;
            return GUEST_OK;
        case 0xF2C0u:                                   /* MOVT */
            c->r[rd] = (c->r[rd] & 0xFFFFu) | (((rn << 12) | imm12) << 16);
            c->r[15] = next;
            return GUEST_OK;
        case 0xF300u:                                   /* SSAT */
        case 0xF380u: {                                 /* USAT */
            uint32_t satimm = hw2 & 0x1Fu;
            uint32_t shift = (((hw2 >> 12) & 7u) << 2) |
                             ((hw2 >> 6) & 3u);
            int usat = ((hw & 0xFBF0u) == 0xF380u);
            int32_t v;
            if (hw2 & 0x20u) {                          /* ASR */
                uint32_t amt = shift ? shift : 32u;
                v = amt >= 32u ? ((int32_t)c->r[rn] >> 31)
                               : ((int32_t)c->r[rn] >> amt);
            } else {                                    /* LSL */
                v = (int32_t)(c->r[rn] << shift);
            }
            if (usat) {
                int32_t hi = satimm >= 31u ? (int32_t)0x7FFFFFFF
                                           : (int32_t)((1u << satimm) - 1u);
                if (v < 0) { v = 0; c->cpsr |= CPSR_Q; }
                else if (v > hi) { v = hi; c->cpsr |= CPSR_Q; }
            } else {
                uint32_t width = satimm + 1u;
                int32_t hi = width >= 32u ? (int32_t)0x7FFFFFFF
                                           : (int32_t)((1u << (width - 1u)) - 1u);
                int32_t lo = width >= 32u ? (int32_t)0x80000000u
                                           : -(int32_t)(1u << (width - 1u));
                if (v < lo) { v = lo; c->cpsr |= CPSR_Q; }
                else if (v > hi) { v = hi; c->cpsr |= CPSR_Q; }
            }
            c->r[rd] = (uint32_t)v;
            c->r[15] = next;
            return GUEST_OK;
        }
        case 0xF340u:                                   /* SBFX */
        case 0xF3C0u: {                                 /* UBFX */
            uint32_t lsb = (((hw2 >> 12) & 7) << 2) | ((hw2 >> 6) & 3);
            uint32_t width = (hw2 & 0x1Fu) + 1u;
            uint32_t v = (lsb + width <= 32)
                       ? ((c->r[rn] >> lsb) & (width == 32 ? 0xFFFFFFFFu
                                                           : ((1u << width) - 1u)))
                       : 0u;
            if ((hw & 0xFBF0u) == 0xF340u && width < 32 && (v & (1u << (width - 1))))
                v |= ~((1u << width) - 1u);             /* sign-extend */
            c->r[rd] = v;
            c->r[15] = next;
            return GUEST_OK;
        }
        case 0xF360u: {                                 /* BFI / BFC */
            uint32_t lsb = (((hw2 >> 12) & 7) << 2) | ((hw2 >> 6) & 3);
            uint32_t msb = hw2 & 0x1Fu;
            if (msb >= lsb) {
                uint32_t width = msb - lsb + 1u;
                uint32_t mask = (width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1u))
                                << lsb;
                uint32_t ins = (rn == 15) ? 0u : (c->r[rn] << lsb);
                c->r[rd] = (c->r[rd] & ~mask) | (ins & mask);
            }
            c->r[15] = next;
            return GUEST_OK;
        }
        default:
            UNDEF(g, pc, insn);
        }
    }

    /* ---- load/store single: hw1 = 1111 100x ----------------------------- */
    if ((hw & 0xFE00u) == 0xF800u) {
        uint32_t rn = hw & 0xF, rt = (hw2 >> 12) & 0xF;
        uint32_t size = (hw >> 5) & 3;        /* 0=byte 1=half 2=word */
        int load = (int)((hw >> 4) & 1);
        int sign = (int)((hw >> 8) & 1);      /* LDRSB/LDRSH live at 0xF9xx */
        uint32_t addr, v;
        int wb = 0;
        uint32_t wb_val = 0;

        if (size == 3)
            UNDEF(g, pc, insn);

        if (rn == 15 && load) {                         /* literal */
            uint32_t imm12 = hw2 & 0xFFFu;
            addr = (hw & 0x0080u) ? lit_pc + imm12 : lit_pc - imm12;
        } else if (hw & 0x0080u) {                      /* imm12 form */
            addr = c->r[rn] + (hw2 & 0xFFFu);
        } else if (hw2 & 0x0800u) {                     /* imm8, P/U/W */
            uint32_t imm8 = hw2 & 0xFFu;
            int P = (int)((hw2 >> 10) & 1), U = (int)((hw2 >> 9) & 1);
            int W = (int)((hw2 >> 8) & 1);
            uint32_t base = c->r[rn];
            uint32_t off = U ? base + imm8 : base - imm8;
            addr = P ? off : base;
            wb = (!P || W);
            wb_val = off;
        } else if ((hw2 & 0x0FC0u) == 0) {              /* register offset */
            int cout;
            addr = c->r[rn] + shift_c(c->r[hw2 & 0xF], SH_LSL,
                                      (int)((hw2 >> 4) & 3), get_c(c), &cout);
        } else {
            UNDEF(g, pc, insn);
        }

        /* T32 encodes the preload hints as narrow loads with Rt=1111 -- PLD is
         * literally "LDRB pc, [Rn]" -- so an Rt=15 load is only a branch at
         * word size. That form is real and the PLT stubs use it; treating a
         * narrow one as a branch loads a data byte into PC, which is how
         * `pld [r0]` over the string "Sun" branched to 0x53. A hint must also
         * not fault, so this returns before the access. */
        if (load && rt == 15 && size != 2) {
            c->r[15] = next;
            return GUEST_OK;
        }

        if (load) {
            int ok = (size == 0) ? guest_ld8(&g->mem, addr, &v)
                   : (size == 1) ? guest_ld16(&g->mem, addr, &v)
                                 : guest_ld32(&g->mem, addr, &v);
            if (!ok)
                MEMFAULT(g, addr);
            if (sign && size == 0)
                v = (uint32_t)(int32_t)(int8_t)v;
            else if (sign && size == 1)
                v = (uint32_t)(int32_t)(int16_t)v;
            if (wb)
                c->r[rn] = wb_val;
            if (rt == 15) {
                branch_interworking(c, v);
                return GUEST_OK;
            }
            c->r[rt] = v;
        } else {
            int ok = (size == 0) ? guest_st8(&g->mem, addr, c->r[rt])
                   : (size == 1) ? guest_st16(&g->mem, addr, c->r[rt])
                                 : guest_st32(&g->mem, addr, c->r[rt]);
            if (!ok)
                MEMFAULT(g, addr);
            if (wb)
                c->r[rn] = wb_val;
        }
        c->r[15] = next;
        return GUEST_OK;
    }

    /* ---- TBB / TBH: table branch. hw1 = 1110 1000 1101 Rn,
     * hw2 = 1111 0000 000H Rm.
     *
     * MUST be tested before the LDRD/STRD block below, whose mask also matches
     * this encoding. Otherwise the table branch decodes as a load, execution
     * falls through into the jump table, and the table's *data* runs as
     * instructions -- which is what happened: a switch dispatched into
     * garbage instead of branching. Always Thumb, so no interworking. */
    if ((hw & 0xFFF0u) == 0xE8D0u && (hw2 & 0xFFE0u) == 0xF000u) {
        uint32_t rn = hw & 0xFu, rm = hw2 & 0xFu;
        uint32_t base = (rn == 15) ? (pc + 4u) : c->r[rn];
        uint32_t addr, v;
        if ((hw2 >> 4) & 1u) {                          /* TBH: halfword table */
            addr = base + 2u * c->r[rm];
            if (!guest_ld16(&g->mem, addr, &v))
                MEMFAULT(g, addr);
        } else {                                        /* TBB: byte table */
            addr = base + c->r[rm];
            if (!guest_ld8(&g->mem, addr, &v))
                MEMFAULT(g, addr);
        }
        c->r[15] = pc + 4u + 2u * v;
        return GUEST_OK;
    }

    /* ---- LDRD / STRD: hw1 = 1110 100P U1W0 Rn --------------------------- */
    if ((hw & 0xFE40u) == 0xE840u) {
        uint32_t rn = hw & 0xF;
        uint32_t rt = (hw2 >> 12) & 0xF, rt2 = (hw2 >> 8) & 0xF;
        int P = (int)((hw >> 8) & 1), U = (int)((hw >> 7) & 1);
        int W = (int)((hw >> 5) & 1), L = (int)((hw >> 4) & 1);
        uint32_t imm = (hw2 & 0xFFu) * 4u;
        uint32_t base = (rn == 15) ? lit_pc : c->r[rn];
        uint32_t off = U ? base + imm : base - imm;
        uint32_t addr = P ? off : base;
        uint32_t a, b;

        if (L) {
            if (!guest_ld32(&g->mem, addr, &a) ||
                !guest_ld32(&g->mem, addr + 4, &b))
                MEMFAULT(g, addr);
            c->r[rt] = a;
            c->r[rt2] = b;
        } else if (!guest_st32(&g->mem, addr, c->r[rt]) ||
                   !guest_st32(&g->mem, addr + 4, c->r[rt2])) {
            MEMFAULT(g, addr);
        }
        if ((!P || W) && rn != 15)
            c->r[rn] = off;
        c->r[15] = next;
        return GUEST_OK;
    }

    /* ---- load/store multiple: hw1 = 1110 100x (PUSH.W/POP.W live here) --- */
    if ((hw & 0xFE40u) == 0xE800u) {
        uint32_t rn = hw & 0xF, list = hw2;
        int load = (int)((hw >> 4) & 1);
        int W = (int)((hw >> 5) & 1);
        int db = (int)((hw >> 8) & 1);        /* 0 = IA, 1 = DB */
        uint32_t base = c->r[rn], addr, n = 0;
        uint32_t branch_to = 0;
        int branched = 0;

        for (int i = 0; i < 16; i++)
            if (list & (1u << i))
                n++;
        addr = db ? base - 4u * n : base;

        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i)))
                continue;
            if (load) {
                uint32_t v;
                if (!guest_ld32(&g->mem, addr, &v))
                    MEMFAULT(g, addr);
                if (i == 15) {
                    branch_to = v;
                    branched = 1;
                } else {
                    c->r[i] = v;
                }
            } else {
                if (!guest_st32(&g->mem, addr, c->r[i]))
                    MEMFAULT(g, addr);
            }
            addr += 4;
        }
        if (W)
            c->r[rn] = db ? base - 4u * n : base + 4u * n;
        if (branched) {
            branch_interworking(c, branch_to);
            return GUEST_OK;
        }
        c->r[15] = next;
        return GUEST_OK;
    }

    /* ---- data processing, shifted register: hw1 = 1110 101x ------------- */
    if ((hw & 0xFE00u) == 0xEA00u) {
        uint32_t op = (hw >> 5) & 0xF, S = (hw >> 4) & 1;
        uint32_t rn = hw & 0xF, rd = (hw2 >> 8) & 0xF, rm = hw2 & 0xF;
        uint32_t amount = (((hw2 >> 12) & 7) << 2) | ((hw2 >> 6) & 3);
        uint32_t type = (hw2 >> 4) & 3;
        int cout = get_c(c);
        uint32_t b;
        int writes;
        uint32_t res;

        if (type == SH_LSR || type == SH_ASR)
            if (amount == 0)
                amount = 32;
        b = shift_c(c->r[rm], (int)type, (int)amount, get_c(c), &cout);
        /* Same Rn=1111 rule as the modified-immediate group above; here it is
         * the register MOV/LSL/LSR/ASR/ROR/RRX and MVN forms. */
        res = dp_compute(c, op, rn == 15 ? 0u : c->r[rn], b,
                         (int)S || rd == 15, cout, &writes);
        if (writes < 0)
            UNDEF(g, pc, insn);
        if (writes && rd != 15)
            c->r[rd] = res;
        c->r[15] = next;
        return GUEST_OK;
    }

    /* ---- data processing (register): hw1 = 1111 1010 --------------------
     * Must be 0xFF00, not 0xFE00: 0xFE00 also swallows 0xFBxx and makes the
     * multiply group below unreachable. */
    if ((hw & 0xFF00u) == 0xFA00u) {
        uint32_t rn = hw & 0xF, rd = (hw2 >> 8) & 0xF, rm = hw2 & 0xF;

        /* shift by register: hw2 = 1111 dddd 0000 mmmm */
        if ((hw & 0xFF80u) == 0xFA00u && (hw2 & 0xF0F0u) == 0xF000u) {
            uint32_t type = (hw >> 5) & 3;
            int cout = get_c(c);
            uint32_t v = shift_c(c->r[rn], (int)type,
                                 (int)(c->r[rm] & 0xFF), get_c(c), &cout);
            c->r[rd] = v;
            if (hw & 0x0010u) {
                set_nz(c, v);
                set_c(c, cout);
            }
            c->r[15] = next;
            return GUEST_OK;
        }

        /* Sign/zero extend, with or without accumulate: hw2 = 1111 dddd 10rr
         * mmmm. Rn == 1111 is the plain form (SXTB/SXTH/UXTB/UXTH); any other
         * Rn is the accumulate form (SXTAB/SXTAH/UXTAB/UXTAH), which adds Rn.
         *
         * The op field is bits 7:4 of hw1 and Thumb numbers these DIFFERENTLY
         * from A32: 0=SXTH 1=UXTH 4=SXTB 5=UXTB. Using A32's numbering here
         * made a Thumb `uxtb` sign-extend, and left UXTH/SXTH undefined. */
        /* Match on the op field, not on Rn: SEL (op 0xA) and CLZ (op 0xB)
         * share the hw2 pattern and have their own handlers below. Ops
         * 0/1/4/5 are exactly those with bits 3 and 1 clear. */
        if ((hw2 & 0xF080u) == 0xF080u && (((hw >> 4) & 0xAu) == 0)) {
            uint32_t rot = ((hw2 >> 4) & 3) * 8;
            uint32_t v = c->r[rm];
            uint32_t res;
            v = rot ? ((v >> rot) | (v << (32 - rot))) : v;
            switch ((hw >> 4) & 0xFu) {
            case 0x0: res = (uint32_t)(int32_t)(int16_t)v; break;   /* SXT(A)H */
            case 0x1: res = v & 0xFFFFu; break;                     /* UXT(A)H */
            case 0x4: res = (uint32_t)(int32_t)(int8_t)v; break;    /* SXT(A)B */
            case 0x5: res = v & 0xFFu; break;                       /* UXT(A)B */
            default: UNDEF(g, pc, insn);
            }
            if (rn != 15)
                res += c->r[rn];
            c->r[rd] = res;
            c->r[15] = next;
            return GUEST_OK;
        }

        /* ARMv6 parallel byte arithmetic. Only the pair the CRT's SIMD strlen
         * needs is implemented; the rest of the group is left UNDEF rather than
         * guessed, because every variant has its own GE rule (carry for the
         * unsigned adds, borrow for the subs, sign for the signed forms, and
         * none at all for the saturating and halving ones).
         * UADD8: hw1 = 1111 1010 1000 nnnn, hw2 = 1111 dddd 0100 mmmm */
        if ((hw & 0xFFF0u) == 0xFA80u && (hw2 & 0xF0F0u) == 0xF040u) {
            uint32_t a = c->r[rn], b = c->r[rm], res = 0, ge = 0, i;
            for (i = 0; i < 4; i++) {
                uint32_t sum = ((a >> (8 * i)) & 0xFFu) +
                               ((b >> (8 * i)) & 0xFFu);
                res |= (sum & 0xFFu) << (8 * i);
                if (sum >= 0x100u)              /* lane carry-out sets GE[i] */
                    ge |= 1u << i;
            }
            c->r[rd] = res;
            c->cpsr = (c->cpsr & ~CPSR_GE_MASK) | (ge << CPSR_GE_SHIFT);
            c->r[15] = next;
            return GUEST_OK;
        }

        /* SEL: hw1 = 1111 1010 1010 nnnn, hw2 = 1111 dddd 1000 mmmm.
         * Per lane: GE[i] ? Rn : Rm. */
        if ((hw & 0xFFF0u) == 0xFAA0u && (hw2 & 0xF0F0u) == 0xF080u) {
            uint32_t ge = (c->cpsr & CPSR_GE_MASK) >> CPSR_GE_SHIFT;
            uint32_t res = 0, i;
            for (i = 0; i < 4; i++) {
                uint32_t src = (ge & (1u << i)) ? c->r[rn] : c->r[rm];
                res |= src & (0xFFu << (8 * i));
            }
            c->r[rd] = res;
            c->r[15] = next;
            return GUEST_OK;
        }

        /* CLZ: hw1 = 1111 1010 1011 nnnn, hw2 = 1111 dddd 1000 mmmm */
        if ((hw & 0xFFF0u) == 0xFAB0u && (hw2 & 0xF0F0u) == 0xF080u) {
            uint32_t v = c->r[rm], n = 0;
            while (n < 32 && !(v & 0x80000000u)) {
                v <<= 1;
                n++;
            }
            c->r[rd] = n;
            c->r[15] = next;
            return GUEST_OK;
        }
        UNDEF(g, pc, insn);
    }

    /* ---- multiply / divide: hw1 = 1111 1011 ----------------------------- */
    if ((hw & 0xFF80u) == 0xFB00u) {
        uint32_t rn = hw & 0xF, rd = (hw2 >> 8) & 0xF, rm = hw2 & 0xF;
        uint32_t ra = (hw2 >> 12) & 0xF;
        if ((hw & 0xFFF0u) == 0xFB00u && (hw2 & 0x00F0u) == 0) {
            c->r[rd] = (ra == 15) ? c->r[rn] * c->r[rm]              /* MUL */
                                  : c->r[ra] + c->r[rn] * c->r[rm];  /* MLA */
            c->r[15] = next;
            return GUEST_OK;
        }
        /* ARMv5TE halfword multiplies, Thumb spelling: hw1 = 1111 1011 0001
         * Rn, hw2 = Ra Rd 00 N M Rm. Ra == 1111 is SMUL<x><y>, otherwise
         * SMLA<x><y>. Note Thumb puts the half selectors in bits 5 (Rn) and
         * 4 (Rm); A32 uses bits 5 and 6. Same family as the A32 block in
         * step_arm -- the CRT and the game use both spellings. */
        if ((hw & 0xFFF0u) == 0xFB10u && (hw2 & 0x00C0u) == 0) {
            int32_t nh = (int32_t)(int16_t)(((hw2 >> 5) & 1u)
                                            ? (c->r[rn] >> 16) : c->r[rn]);
            int32_t mh = (int32_t)(int16_t)(((hw2 >> 4) & 1u)
                                            ? (c->r[rm] >> 16) : c->r[rm]);
            uint32_t prod = (uint32_t)(nh * mh);
            c->r[rd] = (ra == 15) ? prod : c->r[ra] + prod;
            c->r[15] = next;
            return GUEST_OK;
        }

        if ((hw & 0xFFF0u) == 0xFB00u && (hw2 & 0x00F0u) == 0x0010u) {
            c->r[rd] = c->r[ra] - c->r[rn] * c->r[rm];               /* MLS */
            c->r[15] = next;
            return GUEST_OK;
        }
        UNDEF(g, pc, insn);
    }

    if ((hw & 0xFF80u) == 0xFB80u) {
        uint32_t rn = hw & 0xF, rm = hw2 & 0xF;
        uint32_t rdlo = (hw2 >> 12) & 0xF, rdhi = (hw2 >> 8) & 0xF;
        /* op1 is hw[6:4] across this whole space, and the two bits that
         * matter are independent: 0x20 selects unsigned, 0x40 selects
         * accumulate. 000 SMULL, 010 UMULL, 100 SMLAL, 110 UMLAL; 001 SDIV,
         * 011 UDIV. */
        if ((hw2 & 0x00F0u) == 0xF0u) {                              /* SDIV/UDIV */
            uint32_t d = c->r[rm];
            if ((hw & 0x0020u) == 0)                                 /* SDIV */
                c->r[rdhi] = d ? (uint32_t)((int32_t)c->r[rn] / (int32_t)d) : 0;
            else                                                     /* UDIV */
                c->r[rdhi] = d ? c->r[rn] / d : 0;
            c->r[15] = next;
            return GUEST_OK;
        }
        if ((hw2 & 0x00F0u) == 0) {                  /* SMULL/UMULL/SMLAL/UMLAL */
            uint64_t p;
            if (hw & 0x0020u)
                p = (uint64_t)c->r[rn] * (uint64_t)c->r[rm];
            else
                p = (uint64_t)((int64_t)(int32_t)c->r[rn] *
                               (int64_t)(int32_t)c->r[rm]);
            /* The accumulate, which testing only 0x20 dropped outright: SMLAL
             * ran as SMULL and UMLAL as UMULL. Signed and unsigned accumulate
             * identically once the product is 64 bits wide, so this needs no
             * second case. The A32 decoder above always had it. */
            if (hw & 0x0040u)
                p += ((uint64_t)c->r[rdhi] << 32) | c->r[rdlo];
            c->r[rdlo] = (uint32_t)p;
            c->r[rdhi] = (uint32_t)(p >> 32);
            c->r[15] = next;
            return GUEST_OK;
        }
        UNDEF(g, pc, insn);
    }

    UNDEF(g, pc, insn);
}

/* ---------------------------------------------------- Thumb16 classification
 *
 * Every top-level test in step_thumb masks only the top eight bits -- the
 * finest mask used is 0xFF00 -- so hw >> 8 decides the class outright, with no
 * residual test left over. That turns a chain of up to nineteen sequential
 * compares into a single indexed load, and it costs the same whichever class
 * the instruction belongs to, so the rare encodings that used to sit at the
 * bottom of the chain stop being expensive.
 *
 * The table is BUILT by running those predicates rather than transcribed from
 * them. Hand-writing 256 entries from twenty overlapping masks is precisely
 * the sort of edit that looks right and is wrong in one place, and the one
 * place would be some rare encoding that misbehaves much later, a long way
 * from this code. Generated this way it cannot disagree with the chain it
 * replaces -- and the order matters, because several of these masks do overlap
 * and the original chain resolved that by first-match-wins. */
enum {
    TK_UNDEF = 0, TK_T32, TK_SHIFT, TK_IMM8, TK_HIREG, TK_LDRLIT, TK_PUSHPOP,
    TK_CBZ, TK_BCOND, TK_B, TK_ALUREG, TK_LDSTREG, TK_LDSTIMM, TK_LDSTH,
    TK_LDSTSP, TK_ADR, TK_STM, TK_ADDSP, TK_EXTEND, TK_REV, TK_IT
};

static uint8_t g_thumb_kind[256];

static void thumb_kind_init(void) {
    unsigned h;
    for (h = 0; h < 256; h++) {
        uint32_t hw = (uint32_t)h << 8;
        uint8_t k;
        if      ((hw & 0xF800u) >= 0xE800u) k = TK_T32;
        else if ((hw & 0xE000u) == 0x0000u) k = TK_SHIFT;
        else if ((hw & 0xE000u) == 0x2000u) k = TK_IMM8;
        else if ((hw & 0xFC00u) == 0x4400u) k = TK_HIREG;
        else if ((hw & 0xF800u) == 0x4800u) k = TK_LDRLIT;
        else if ((hw & 0xF600u) == 0xB400u) k = TK_PUSHPOP;
        else if ((hw & 0xF500u) == 0xB100u) k = TK_CBZ;
        else if ((hw & 0xF000u) == 0xD000u) k = TK_BCOND;
        else if ((hw & 0xF800u) == 0xE000u) k = TK_B;
        else if ((hw & 0xFC00u) == 0x4000u) k = TK_ALUREG;
        else if ((hw & 0xF000u) == 0x5000u) k = TK_LDSTREG;
        else if ((hw & 0xE000u) == 0x6000u) k = TK_LDSTIMM;
        else if ((hw & 0xF000u) == 0x8000u) k = TK_LDSTH;
        else if ((hw & 0xF000u) == 0x9000u) k = TK_LDSTSP;
        else if ((hw & 0xF000u) == 0xA000u) k = TK_ADR;
        else if ((hw & 0xF000u) == 0xC000u) k = TK_STM;
        else if ((hw & 0xFF00u) == 0xB000u) k = TK_ADDSP;
        else if ((hw & 0xFF00u) == 0xB200u) k = TK_EXTEND;
        else if ((hw & 0xFF00u) == 0xBA00u) k = TK_REV;
        else if ((hw & 0xFF00u) == 0xBF00u) k = TK_IT;
        else                                k = TK_UNDEF;
        g_thumb_kind[h] = k;
    }
}

static inline uint8_t thumb_kind(uint32_t hw) {
    return g_thumb_kind[(hw >> 8) & 0xFFu];
}

/* -------------------------------------------------------------- Thumb decode */

static GuestStatus step_thumb(Guest *g) {
    GuestCpu *c = &g->cpu;
    uint32_t pc = c->r[15], hw;

    if (!guest_ifetch16(&g->mem, pc, &hw))
        MEMFAULT(g, pc);
    if (g->iprof)
        g->iprof[((hw & 0xF800u) >= 0xE800u ? 256u : 0u) + (hw >> 8)]++;

    const uint32_t read_pc = pc + 4;
    const uint32_t lit_pc = (pc + 4) & ~3u;   /* Align(PC,4) for literal loads */

    /* A 16-bit data-processing instruction sets flags only OUTSIDE an IT
     * block: `0x3004` is `adds r0,#4` standalone but a plain `add` under
     * `itt`. Setting them anyway lets an IT block destroy its own condition
     * midway -- `addeq r0,#4` cleared Z, so the `moveq` after it was skipped.
     * ITSTATE is consumed just below, before the handlers run, so latch it
     * here. CMP/CMN/TST are exempt: setting flags is their whole purpose. */
    const int in_it = (c->itstate & 0x0Fu) != 0;

    if (c->itstate & 0x0Fu) {
        uint32_t itcond = (c->itstate >> 4) & 0xF;
        int exec = cond_ok(c->cpsr, itcond);
        /* advance ITSTATE for this instruction either way */
        c->itstate = ((c->itstate & 7u) == 0)
                   ? 0u
                   : ((c->itstate & 0xE0u) | ((c->itstate << 1) & 0x1Fu));
        if (!exec) {
            c->r[15] = pc + (((hw & 0xF800u) >= 0xE800u) ? 4u : 2u);
            return GUEST_OK;
        }
    }

    switch (thumb_kind(hw)) {
    case TK_T32: {                    /* 32-bit Thumb-2 */
        uint32_t hw2;
        if (!guest_ifetch16(&g->mem, pc + 2, &hw2))
            MEMFAULT(g, pc + 2);

        return step_thumb32(g, pc, hw, hw2);
    }
    break;

    case TK_SHIFT: {          /* shift imm / add / sub */
        uint32_t op = (hw >> 11) & 3;
        uint32_t rd = hw & 7, rm = (hw >> 3) & 7, imm = (hw >> 6) & 0x1F;
        if (op != 3) {
            int cout = get_c(c);
            int amt = (op == SH_LSL) ? (int)imm : (imm ? (int)imm : 32);
            uint32_t v = shift_c(c->r[rm], (int)op, amt, get_c(c), &cout);
            c->r[rd] = v;
            if (!in_it) {
                set_nz(c, v);
                set_c(c, cout);
            }
        } else {
            uint32_t rn = (hw >> 6) & 7;
            uint32_t b = (hw & 0x0400u) ? rn : c->r[rn];
            c->r[rd] = (hw & 0x0200u)
                     ? add_with_carry(c, c->r[rm], ~b, 1, !in_it)
                     : add_with_carry(c, c->r[rm], b, 0, !in_it);
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_IMM8: {          /* MOV/CMP/ADD/SUB imm8 */
        uint32_t op = (hw >> 11) & 3, rd = (hw >> 8) & 7, imm = hw & 0xFF;
        switch (op) {
        case 0: c->r[rd] = imm; if (!in_it) set_nz(c, imm); break;
        case 1: add_with_carry(c, c->r[rd], ~imm, 1, 1); break;  /* CMP */
        case 2: c->r[rd] = add_with_carry(c, c->r[rd], imm, 0, !in_it); break;
        default: c->r[rd] = add_with_carry(c, c->r[rd], ~imm, 1, !in_it); break;
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_HIREG: {          /* hi-reg ADD/CMP/MOV, BX/BLX */
        uint32_t op = (hw >> 8) & 3;
        uint32_t rd = (uint32_t)(((hw >> 4) & 8) | (hw & 7));
        uint32_t rm = (hw >> 3) & 0xF;
        uint32_t v = (rm == 15) ? read_pc : c->r[rm];
        switch (op) {
        case 0:
            if (rd == 15) {
                c->r[15] = (read_pc + v) & ~1u;
                return GUEST_OK;
            }
            c->r[rd] += v;
            break;
        case 1:
            add_with_carry(c, (rd == 15) ? read_pc : c->r[rd], ~v, 1, 1);
            break;
        case 2:
            if (rd == 15) {
                branch_interworking(c, v);
                return GUEST_OK;
            }
            c->r[rd] = v;
            break;
        default:
            if (hw & 0x0080u)
                c->r[14] = (pc + 2) | 1u;
            branch_interworking(c, v);
            return GUEST_OK;
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_LDRLIT: {          /* LDR literal */
        uint32_t rd = (hw >> 8) & 7, addr = lit_pc + ((hw & 0xFFu) << 2), v;
        if (!guest_ld32(&g->mem, addr, &v))
            MEMFAULT(g, addr);
        c->r[rd] = v;
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_PUSHPOP: {          /* PUSH / POP */
        int load = (int)((hw >> 11) & 1);
        uint32_t list = hw & 0xFF;
        int extra = (int)((hw >> 8) & 1);
        int n = extra;
        for (int i = 0; i < 8; i++)
            if (list & (1u << i))
                n++;

        if (load) {
            uint32_t sp = c->r[GUEST_SP];
            for (int i = 0; i < 8; i++) {
                uint32_t v;
                if (!(list & (1u << i)))
                    continue;
                if (!guest_ld32(&g->mem, sp, &v))
                    MEMFAULT(g, sp);
                c->r[i] = v;
                sp += 4;
            }
            if (extra) {
                uint32_t v;
                if (!guest_ld32(&g->mem, sp, &v))
                    MEMFAULT(g, sp);
                c->r[GUEST_SP] = sp + 4;
                branch_interworking(c, v);
                return GUEST_OK;
            }
            c->r[GUEST_SP] = sp;
        } else {
            uint32_t sp = c->r[GUEST_SP] - 4u * (uint32_t)n, a = sp;
            for (int i = 0; i < 8; i++) {
                if (!(list & (1u << i)))
                    continue;
                if (!guest_st32(&g->mem, a, c->r[i]))
                    MEMFAULT(g, a);
                a += 4;
            }
            if (extra && !guest_st32(&g->mem, a, c->r[GUEST_LR]))
                MEMFAULT(g, a);
            c->r[GUEST_SP] = sp;
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_CBZ: {          /* CBZ / CBNZ */
        uint32_t rn = hw & 7;
        uint32_t imm = (uint32_t)((((hw >> 9) & 1) << 6) | (((hw >> 3) & 0x1F) << 1));
        int nonzero = (int)((hw >> 11) & 1);
        if ((c->r[rn] != 0) == nonzero) {
            c->r[15] = read_pc + imm;
            return GUEST_OK;
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_BCOND: {          /* conditional branch */
        uint32_t cond = (hw >> 8) & 0xF;
        if (cond >= 0xE)
            UNDEF(g, pc, hw);                  /* UDF / SVC: not needed here */
        if (cond_ok(c->cpsr, cond)) {
            int32_t off = (int32_t)(int8_t)(hw & 0xFF) * 2;
            c->r[15] = read_pc + (uint32_t)off;
            return GUEST_OK;
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_B: {          /* unconditional branch */
        int32_t off = (int32_t)((hw & 0x7FFu) << 21) >> 20;
        c->r[15] = read_pc + (uint32_t)off;
        return GUEST_OK;
    }
    break;

    case TK_ALUREG: {          /* ALU register operations */
        uint32_t op = (hw >> 6) & 0xF, rd = hw & 7, rm = (hw >> 3) & 7;
        uint32_t a = c->r[rd], b = c->r[rm], res;
        int cout = get_c(c), logical = 1, writes = 1;
        switch (op) {
        case 0x0: res = a & b; break;                                /* AND */
        case 0x1: res = a ^ b; break;                                /* EOR */
        case 0x2: res = shift_c(a, SH_LSL, (int)(b & 0xFF), cout, &cout); break;
        case 0x3: res = shift_c(a, SH_LSR, (int)(b & 0xFF), cout, &cout); break;
        case 0x4: res = shift_c(a, SH_ASR, (int)(b & 0xFF), cout, &cout); break;
        case 0x5: res = add_with_carry(c, a, b, get_c(c), !in_it); logical = 0; break;
        case 0x6: res = add_with_carry(c, a, ~b, get_c(c), !in_it); logical = 0; break;
        case 0x7: res = shift_c(a, SH_ROR, (int)(b & 0xFF), cout, &cout); break;
        case 0x8: res = a & b; writes = 0; break;                    /* TST */
        case 0x9: res = add_with_carry(c, ~b, 0, 1, !in_it); logical = 0; break; /* RSB */
        case 0xA: res = add_with_carry(c, a, ~b, 1, 1); logical = 0; writes = 0; break;
        case 0xB: res = add_with_carry(c, a, b, 0, 1); logical = 0; writes = 0; break;
        case 0xC: res = a | b; break;                                /* ORR */
        case 0xD: res = a * b; break;                                /* MUL */
        case 0xE: res = a & ~b; break;                               /* BIC */
        default:  res = ~b; break;                                   /* MVN */
        }
        /* TST (0x8) is a compare: it sets flags even inside an IT block. */
        if (logical && (!in_it || op == 0x8)) {
            set_nz(c, res);
            if (op != 0xD)
                set_c(c, cout);
        }
        if (writes)
            c->r[rd] = res;
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_LDSTREG: {          /* load/store register offset */
        uint32_t op = (hw >> 9) & 7, rd = hw & 7;
        uint32_t addr = c->r[(hw >> 3) & 7] + c->r[(hw >> 6) & 7];
        uint32_t v;
        switch (op) {
        case 0: if (!guest_st32(&g->mem, addr, c->r[rd])) MEMFAULT(g, addr); break;
        case 1: if (!guest_st16(&g->mem, addr, c->r[rd])) MEMFAULT(g, addr); break;
        case 2: if (!guest_st8(&g->mem, addr, c->r[rd])) MEMFAULT(g, addr); break;
        case 3: if (!guest_ld8(&g->mem, addr, &v)) MEMFAULT(g, addr);
                c->r[rd] = (uint32_t)(int32_t)(int8_t)v; break;
        case 4: if (!guest_ld32(&g->mem, addr, &v)) MEMFAULT(g, addr);
                c->r[rd] = v; break;
        case 5: if (!guest_ld16(&g->mem, addr, &v)) MEMFAULT(g, addr);
                c->r[rd] = v; break;
        case 6: if (!guest_ld8(&g->mem, addr, &v)) MEMFAULT(g, addr);
                c->r[rd] = v; break;
        default: if (!guest_ld16(&g->mem, addr, &v)) MEMFAULT(g, addr);
                c->r[rd] = (uint32_t)(int32_t)(int16_t)v; break;
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_LDSTIMM: {          /* load/store word/byte imm5 */
        int B = (int)((hw >> 12) & 1), L = (int)((hw >> 11) & 1);
        uint32_t imm = (hw >> 6) & 0x1F, rd = hw & 7;
        uint32_t addr = c->r[(hw >> 3) & 7] + (B ? imm : imm * 4u);
        uint32_t v;
        if (L) {
            if (B ? !guest_ld8(&g->mem, addr, &v) : !guest_ld32(&g->mem, addr, &v))
                MEMFAULT(g, addr);
            c->r[rd] = v;
        } else if (B ? !guest_st8(&g->mem, addr, c->r[rd])
                     : !guest_st32(&g->mem, addr, c->r[rd])) {
            MEMFAULT(g, addr);
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_LDSTH: {          /* load/store halfword imm5 */
        int L = (int)((hw >> 11) & 1);
        uint32_t addr = c->r[(hw >> 3) & 7] + ((hw >> 6) & 0x1Fu) * 2u;
        uint32_t rd = hw & 7, v;
        if (L) {
            if (!guest_ld16(&g->mem, addr, &v))
                MEMFAULT(g, addr);
            c->r[rd] = v;
        } else if (!guest_st16(&g->mem, addr, c->r[rd])) {
            MEMFAULT(g, addr);
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_LDSTSP: {          /* SP-relative load/store */
        int L = (int)((hw >> 11) & 1);
        uint32_t rd = (hw >> 8) & 7;
        uint32_t addr = c->r[GUEST_SP] + (hw & 0xFFu) * 4u;
        uint32_t v;
        if (L) {
            if (!guest_ld32(&g->mem, addr, &v))
                MEMFAULT(g, addr);
            c->r[rd] = v;
        } else if (!guest_st32(&g->mem, addr, c->r[rd])) {
            MEMFAULT(g, addr);
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_ADR: {          /* ADR / ADD Rd, SP, #imm */
        uint32_t rd = (hw >> 8) & 7, imm = (hw & 0xFFu) * 4u;
        c->r[rd] = (hw & 0x0800u) ? c->r[GUEST_SP] + imm : lit_pc + imm;
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_STM: {          /* STMIA / LDMIA */
        int L = (int)((hw >> 11) & 1);
        uint32_t rn = (hw >> 8) & 7, list = hw & 0xFF;
        uint32_t addr = c->r[rn];
        for (int i = 0; i < 8; i++) {
            uint32_t v;
            if (!(list & (1u << i)))
                continue;
            if (L) {
                if (!guest_ld32(&g->mem, addr, &v))
                    MEMFAULT(g, addr);
                c->r[i] = v;
            } else if (!guest_st32(&g->mem, addr, c->r[i])) {
                MEMFAULT(g, addr);
            }
            addr += 4;
        }
        if (!L || !(list & (1u << rn)))
            c->r[rn] = addr;
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_ADDSP: {          /* ADD / SUB SP, #imm7 */
        uint32_t imm = (hw & 0x7Fu) * 4u;
        c->r[GUEST_SP] += (hw & 0x0080u) ? (uint32_t)-(int32_t)imm : imm;
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_EXTEND: {          /* SXTH / SXTB / UXTH / UXTB */
        uint32_t rd = hw & 7, v = c->r[(hw >> 3) & 7];
        switch ((hw >> 6) & 3) {
        case 0: c->r[rd] = (uint32_t)(int32_t)(int16_t)v; break;
        case 1: c->r[rd] = (uint32_t)(int32_t)(int8_t)v; break;
        case 2: c->r[rd] = v & 0xFFFFu; break;
        default: c->r[rd] = v & 0xFFu; break;
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_REV: {          /* REV / REV16 / REVSH */
        uint32_t rd = hw & 7, v = c->r[(hw >> 3) & 7];
        switch ((hw >> 6) & 3) {
        case 0:
            c->r[rd] = ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) |
                       ((v << 8) & 0xFF0000u) | (v << 24);
            break;
        case 1:
            c->r[rd] = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
            break;
        default:
            c->r[rd] = (uint32_t)(int32_t)(int16_t)(uint16_t)
                       (((v >> 8) & 0xFFu) | ((v << 8) & 0xFF00u));
            break;
        }
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;

    case TK_IT: {          /* IT and hints */
        if (hw & 0x000Fu)
            c->itstate = hw & 0xFFu;          /* IT: cond<7:4>:mask<3:0> */
        /* else NOP / YIELD / WFE / WFI / SEV: nothing to do */
        c->r[15] = pc + 2;
        return GUEST_OK;
    }
    break;
    }

    UNDEF(g, pc, hw);
}

/* ------------------------------------------------------------------- driver */

static GuestStatus dispatch_stub(Guest *g) {
    uint32_t idx = guest_stub_index(g->cpu.r[15]);
    GuestHleSlot *s;

    if (idx >= g->hle.count)
        UNDEF(g, g->cpu.r[15], 0);
    s = &g->hle.slot[idx];
    if (g->prof)
        g->prof(idx, 1);
    if (s->fn)
        s->fn(&g->cpu, &g->mem, s->user ? s->user : g->hle.user);
    else
        g->cpu.r[0] = 0;      /* unimplemented import behaves as a no-op stub */
    if (g->prof)
        g->prof(idx, 0);
    branch_interworking(&g->cpu, g->cpu.r[GUEST_LR]);
    return GUEST_OK;
}

/* Re-run a block the JIT just executed, through the interpreter, and report
 * whether the two agree. `before` is the register state the block started
 * from; g->cpu currently holds the JIT's result. See Guest::jit_verify for why
 * the device has to be the oracle here, and for the standing precondition that
 * compiled blocks contain no stores.
 *
 * Registers only, plus CPSR. That is what a block's lowering can get wrong in
 * a way this can catch: a shifter carry not merged into C, an IT block advanced
 * wrongly, a writeback register not updated. Memory is not compared because
 * nothing compiled writes it yet -- when that changes this needs an undo log,
 * not a wider comparison. */
/* The block's writes, taken out of the shared log before the interpreter's
 * re-run starts appending to it. */
static GuestUndo jit_undo_snapshot[GUEST_UNDO_MAX];

static int jit_verify_block(Guest *g, const GuestCpu *before, uint32_t retired) {
    GuestCpu jitted = g->cpu;
    uint32_t nwrote = g->mem.undo_n;
    uint32_t i;
    int bad = -1;

    /* Roll the block's stores back so the interpreter re-runs against the same
     * memory it started from. Without this a block doing ldr/add/str on one
     * address makes the re-run read what the JIT just wrote and compute a
     * different answer -- a divergence manufactured by the check itself. The
     * JIT's values are captured first, since they are what gets compared. */
    if (nwrote > GUEST_UNDO_MAX)
        nwrote = GUEST_UNDO_MAX;
    for (i = 0; i < nwrote; i++) {
        GuestUndo *u = &guest_undo_log[i];
        u->val = 0;
        (void)guest_ld32(&g->mem, u->addr, &u->val);
        if (u->size < 4u)
            u->val &= (1u << (u->size * 8u)) - 1u;
        jit_undo_snapshot[i] = *u;      /* the re-run reuses the log */
    }
    for (i = nwrote; i-- > 0; ) {
        GuestUndo *u = &jit_undo_snapshot[i];
        switch (u->size) {
        case 1:  (void)guest_st8(&g->mem, u->addr, u->old);  break;
        case 2:  (void)guest_st16(&g->mem, u->addr, u->old); break;
        default: (void)guest_st32(&g->mem, u->addr, u->old); break;
        }
    }
    g->mem.undo_n = 0;
    g->cpu = *before;
    for (i = 0; i < retired; i++) {
        GuestStatus st = guest_is_thumb(&g->cpu) ? step_thumb(g) : step_arm(g);
        if (st != GUEST_OK) {
            printf("  [jitv ] block %08x: interpreter faulted at step %u\n",
                   (unsigned)before->r[15], (unsigned)i);
            g->cpu = jitted;
            return 0;
        }
    }
    for (i = 0; i < 16; i++)
        if (g->cpu.r[i] != jitted.r[i]) { bad = (int)i; break; }
    if (bad < 0 && (g->cpu.cpsr & 0xF8000000u) != (jitted.cpsr & 0xF8000000u))
        bad = 16;

    /* Memory now holds the interpreter's stores. Anywhere the JIT wrote should
     * agree; where it wrote and the interpreter did not, the restored original
     * still stands and disagrees with what the JIT left, which is the same
     * signal. A location the interpreter wrote and the JIT did not is not
     * caught here, but that almost always shows up in the registers first. */
    if (bad < 0) {
        for (i = 0; i < nwrote; i++) {
            GuestUndo *u = &jit_undo_snapshot[i];
            uint32_t now = 0;
            if (!guest_ld32(&g->mem, u->addr, &now))
                continue;
            if (u->size < 4u)
                now &= (1u << (u->size * 8u)) - 1u;
            if (now != u->val) {
                g->jit_diverged++;
                if (g->jit_diverged <= 20)
                    printf("  [jitv ] block %08x: mem[%08x]/%u jit=%08x "
                           "interp=%08x\n",
                           (unsigned)before->r[15], (unsigned)u->addr,
                           (unsigned)u->size, (unsigned)u->val, (unsigned)now);
                g->mem.undo_n = 0;
                return 0;
            }
        }
    }
    g->mem.undo_n = 0;

    if (bad >= 0) {
        g->jit_diverged++;
        if (g->jit_diverged <= 20) {
            if (bad == 16)
                printf("  [jitv ] block %08x (%u insns): cpsr jit=%08x interp=%08x\n",
                       (unsigned)before->r[15], (unsigned)retired,
                       (unsigned)(jitted.cpsr & 0xF8000000u),
                       (unsigned)(g->cpu.cpsr & 0xF8000000u));
            else
                printf("  [jitv ] block %08x (%u insns): r%d jit=%08x interp=%08x\n",
                       (unsigned)before->r[15], (unsigned)retired, bad,
                       (unsigned)jitted.r[bad], (unsigned)g->cpu.r[bad]);
        }
        /* Keep the INTERPRETER's state, not the JIT's. A divergence means the
         * compiled block is wrong, so continuing from its answer would carry
         * the fault forward into everything after it -- and the point of a
         * verification run is to survive long enough to find more than one. */
        return 0;
    }
    g->cpu = jitted;
    g->jit_verify_blocks++;
    return 1;
}

/* ---- T16 MOV high-register fast path (predecode spike) -----------------
 *
 * 0x46xx is 9% of the in-game instruction stream -- the single largest entry
 * in the mix, ahead of LDR immediate at 7%. It is also one of the cheapest
 * instructions in the ISA: copy one register to another, no flags, no memory.
 * Which is the whole argument for predecoding: at ~105 cycles per guest
 * instruction, essentially all of that is dispatch, not work.
 *
 * What the normal path costs for this instruction: a call into step_thumb
 * (7.3 KB, not inlined), a reload of r15, the instruction fetch, a load from
 * g_thumb_kind[], a 20-way switch through a jump table -- the worst-predicted
 * branch in the loop -- and only then three lines of actual semantics.
 *
 * This spike skips all of it and keeps only the fetch. It deliberately does
 * NOT build the predecode cache: the point is to find out whether removing
 * the call, the kind load and the switch is worth anything on this core
 * BEFORE committing weeks to a cache. Your ledger records that removing five
 * to eight independent host instructions per guest instruction measured
 * exactly 0.0%, because an out-of-order A57 hides them in stalls it takes
 * anyway. Only shortening a dependency chain has ever paid here.
 *
 * The cost side is honest and measurable: every Thumb instruction that is NOT
 * 0x46xx now pays one extra guest_ifetch16, because step_thumb fetches again.
 * That is 91% of the stream paying a duplicated L1 load to save 9% a call and
 * a mispredict. If the result is positive, the full cache -- which removes the
 * duplicate fetch and covers the other formats -- is clearly worth building.
 * If it is zero or negative, the approach is answered for a day's work rather
 * than a month's.
 *
 * Semantics replicated exactly from TK_HIREG op 2: rd from bit 7 and bits 2-0,
 * rm from bits 6-3, the value being pc+4 when rm is 15 (Align is not applied
 * for MOV) and r[rm] otherwise, no flag update, pc advancing by 2.
 *
 * Excluded from the fast path, each falling through to step_thumb unchanged:
 *   - rd == 15, which is a branch with interworking, not a move
 *   - itstate != 0, so the IT machinery is never bypassed
 *   - a failed fetch, which must fault through the normal path
 */
uint64_t g_fastpath_hits, g_fastpath_miss;

/* Direct-mapped "could this PC be a hook" filter, and the hook_count it
 * was built for. File scope rather than a Guest field to leave that
 * struct's layout alone -- the fields the interpreter touches per access
 * are on cache lines worth not disturbing for 256 bytes of table. */
static uint8_t  g_hook_map[256];
static uint32_t g_hook_map_for = 0xFFFFFFFFu;

/* Off unless predecode.txt is on the card.
 *
 * The point of the flag is that both arms of a measurement then run the
 * SAME BINARY. Comparing two builds left the frame counts free to differ --
 * the control landed on 420 frames in the gameplay window where every other
 * run gave 475 -- and a window whose frame count does not match its pair is
 * not a comparison at all. With one binary the guest workload is identical
 * by construction and the only variable is this flag. */
int g_predecode;

GuestStatus guest_run(Guest *g, uint32_t until, uint64_t limit) {
    uint64_t start = g->executed;
    /* Reject non-hook PCs with one table lookup.
     *
     * This was a span test -- lowest hook address to highest -- on the theory
     * that hooks sit far apart and almost every PC falls outside. That held
     * while the hooks were a handful of game functions. It stopped holding as
     * soon as library routines were hooked: the span grew to 2.4 MB of a 7 MB
     * image, and the profiler showed a third of the hot code sitting inside it,
     * paying an eleven-iteration linear scan on every single instruction.
     *
     * A direct-mapped byte table has no such failure mode. A PC whose bucket is
     * clear cannot be a hook, whatever the addresses are, so the cost per
     * instruction stays one AND, one load and one branch no matter how many
     * hooks are installed or where they are. Buckets are keyed on pc>>1 since
     * Thumb entry points are 2-byte aligned. Collisions only cost a scan that
     * finds nothing, and 11 hooks in 256 buckets make even that rare.
     *
     * Built per guest_run call rather than cached in Guest so adding or moving
     * a hook cannot leave a stale table behind; guest_run is entered once per
     * 5M instructions on the device, so 256 bytes of memset is free. */
    /* The ring index lives in a register for the duration of the run and is
     * written back on the way out. It used to be re-loaded from and stored to
     * g->hist_pos on every guest instruction, which measured 6.4% of
     * interpreter time on the host bench -- for a 16-entry crash-forensics
     * buffer. The ring itself is worth keeping; reading its index from memory
     * a hundred million times a second was not. */
    uint32_t hp = g->hist_pos;
    /* Hoisted so the hot loop tests a register rather than reloading a field.
     * The compiler cannot do this itself: handlers called from inside the loop
     * are opaque to it, so it must assume any of them could set g->jit. The
     * JIT is enabled once at startup, before the first guest_run, so a local
     * copy cannot go stale within a call -- and if it ever could, the worst
     * outcome is that the JIT starts one guest_run later than it might have.
     * Left as a plain load-and-branch this measured 2.6% on the host bench,
     * paid by every build whether the JIT was on or not. */
#ifdef BOZ_JIT
    void *jitctx = g->jit;
#endif
    /* Hoisted for the run, for the same reason jitctx is: handlers called
     * from inside this loop are opaque, so the compiler must assume any of
     * them could change these and reload on every guest instruction.
     * Between them that was four loads and a read-modify-write per
     * instruction on the one path every instruction takes. guest_run is
     * entered once per few million instructions, so a stale copy is at
     * worst one run late -- hooks are installed at startup and the watch is
     * a bring-up tool. */
    uint32_t *pcprof = g->pcprof;
    uint32_t pcprof_base = g->pcprof_base;
    uint32_t pcprof_buckets = g->pcprof_buckets;
    uint32_t watch_addr = g->mem.watch_addr;
    uint32_t hook_count = g->hook_count;
    /* Hoisted for the run, as jitctx and pcprof are: set once at startup
     * from the card, so a stale copy is at worst one guest_run late, and
     * the hot path tests a register instead of reloading a global. */
    int predecode = g_predecode;
    /* Once per guest_run, not once per instruction: this is entered every few
     * million instructions, so the guard costs nothing measurable and no call
     * site has to remember to initialise the interpreter. */
    static int kinds_ready;
    if (!kinds_ready) {
        thumb_kind_init();
        kinds_ready = 1;
    }
    /* Built once, not once per guest_run.
     *
     * This was a 256-byte memset plus a loop over every hook on every entry to
     * guest_run -- free when that happens once per 5M instructions, which is
     * what it did when it was written. It stopped being free the moment
     * recomp_call started re-entering guest_run for every outbound call a
     * translated function makes: a list walk with a virtual call per element
     * pays the whole setup per element, against the seven interpreted
     * instructions the translation was supposed to save.
     *
     * Keyed on hook_count because hooks are installed once during startup and
     * never move afterwards. The `observe` flag does change at runtime -- both
     * hook_recomp and the fast-path decline path toggle it -- but observe is
     * not part of this map, only the addresses are. */
    if (hook_count && g_hook_map_for != hook_count) {
        uint32_t i;
        memset(g_hook_map, 0, sizeof g_hook_map);
        for (i = 0; i < hook_count; i++)
            g_hook_map[((g->hook[i].addr & ~1u) >> 1) & 0xFFu] = 1;
        g_hook_map_for = hook_count;
    }

    while (g->executed - start < limit) {
        uint32_t pc = g->cpu.r[15];
        GuestStatus st;

        if ((pc & ~1u) == (until & ~1u)) {
            g->hist_pos = hp;
            return GUEST_HALTED;
        }

        g->hist[hp++ & 15u] = pc;
        /* Only stamp the PC when something is actually watching for it. The
         * watch is a bring-up tool for finding what corrupts a guest word;
         * paying a store per instruction to keep it ready cost 2.8%. */
        if (watch_addr)
            g->mem.current_pc = pc;

        if (pcprof) {
            uint32_t off = ((pc & ~1u) - pcprof_base) >> 4;
            if (off < pcprof_buckets)
                pcprof[off]++;
        }

        if (hook_count && g_hook_map[(pc >> 1) & 0xFFu]) {
            uint32_t hi;
            for (hi = 0; hi < hook_count; hi++) {
                if ((g->hook[hi].addr & ~1u) != pc)
                    continue;
                g->hook[hi].fn(&g->cpu, &g->mem, g->hook[hi].user);
                if (!g->hook[hi].observe)
                    branch_interworking(&g->cpu, g->cpu.r[GUEST_LR]);
                break;
            }
            if (hi < hook_count && !g->hook[hi].observe) {
                g->executed++;
                continue;
            }
        }

#ifdef BOZ_JIT
        /* The JIT gets first refusal on every PC. It declines whenever the
         * block is not compiled, not hot yet, or contains anything without an
         * exact lowering, and declining is free of consequence: execution
         * simply continues here at the same PC. */
        if (jitctx) {
            uint32_t retired = 0;
            int ran;
            /* Snapshot the register file only when something will compare
             * it. This is a 68-byte copy and it was being made on every
             * dispatch whether verifying or not -- the compiler cannot sink
             * it past the call, because the call can read g->cpu. At three
             * guest instructions per block that copy was a real share of
             * what the JIT had to earn back before it broke even. */
            GuestCpu before;
            if (g->jit_verify)
                before = g->cpu;
            /* Record stores only while the block itself is running.
             *
             * Arming this once at startup was wrong in a way that took a
             * corrupted run to see: guest_wptr then logs every store the
             * INTERPRETER makes as well, so by the time a block was checked
             * the log held thousands of unrelated writes, and rolling it back
             * undid ordinary execution rather than the block. The log is also
             * reset here so the interpreter's re-run cannot overwrite the
             * entries the comparison still needs. */
            if (g->jit_verify) {
                g->mem.undo_n = 0;
                g->mem.undo_active = 1;
            }
            ran = guest_jit_try_run(g, until, limit - (g->executed - start),
                                    &retired);
            g->mem.undo_active = 0;
            if (ran && retired) {
                if (!g->jit_verify || jit_verify_block(g, &before, retired)) {
                    g->executed += retired;
                    continue;
                }
                /* Verification failed: the interpreter has already re-executed
                 * the block and its state stands, so the instructions still
                 * retired and must be counted. */
                g->executed += retired;
                continue;
            }
        }
#endif

        if (guest_is_stub(pc)) {
            st = dispatch_stub(g);
        } else if (guest_is_thumb(&g->cpu)) {
            uint32_t hw;
            if (predecode && !g->cpu.itstate &&
                guest_ifetch16(&g->mem, pc, &hw) &&
                (hw & 0xFF00u) == 0x4600u) {
                uint32_t rd = ((hw >> 4) & 8u) | (hw & 7u);
                if (rd != 15u) {
                    uint32_t rm = (hw >> 3) & 0xFu;
                    /* step_thumb would have counted this one; keep the mix
                     * report honest rather than silently hiding 9% of it. */
                    if (g->iprof)
                        g->iprof[hw >> 8]++;
                    g->cpu.r[rd] = (rm == 15u) ? (pc + 4u) : g->cpu.r[rm];
                    g->cpu.r[15] = pc + 2u;
                    g->executed++;
                    g_fastpath_hits++;
                    continue;
                }
            }
            g_fastpath_miss++;
            st = step_thumb(g);
        } else {
            st = step_arm(g);
        }

        if (st != GUEST_OK) {
            g->hist_pos = hp;
            return st;
        }
        g->executed++;
    }
    g->hist_pos = hp;
    return GUEST_STEP_LIMIT;
}

GuestStatus guest_call(Guest *g, uint32_t fn, uint32_t r0, uint32_t r1) {
    GuestCpu saved = g->cpu;
    const uint32_t pad = GUEST_STUB_BASE + GUEST_STUB_SIZE - 4;
    GuestStatus st;

    g->cpu.r[0] = r0;
    g->cpu.r[1] = r1;
    g->cpu.r[GUEST_LR] = pad;
    branch_interworking(&g->cpu, fn);

    st = guest_run(g, pad, 20000000);
    g->cpu = saved;
    return (st == GUEST_HALTED) ? GUEST_OK : st;
}

/* guest_call, keeping the callee's r0. guest_call restores the whole
 * interrupted register state -- r0 included -- which is right for event
 * callbacks nobody reads a result from, and loses the one number a sound
 * generator callback returns: how many samples it produced. */
GuestStatus guest_call_r0(Guest *g, uint32_t fn, uint32_t r0, uint32_t r1,
                          uint32_t *ret) {
    GuestCpu saved = g->cpu;
    const uint32_t pad = GUEST_STUB_BASE + GUEST_STUB_SIZE - 4;
    GuestStatus st;

    g->cpu.r[0] = r0;
    g->cpu.r[1] = r1;
    g->cpu.r[GUEST_LR] = pad;
    branch_interworking(&g->cpu, fn);

    st = guest_run(g, pad, 20000000);
    if (ret)
        *ret = g->cpu.r[0];
    g->cpu = saved;
    return (st == GUEST_HALTED) ? GUEST_OK : st;
}
