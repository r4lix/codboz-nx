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

#else

#include <switch.h>

#define JIT_CODE_SIZE   (8u << 20)
#define JIT_CACHE_SLOTS 32768u
#define JIT_HOT_COUNT   3u
#define JIT_MAX_GUEST   48u
#define JIT_MAX_A64     768u

typedef uint32_t (*JitBlockFn)(Guest *g);

typedef struct {
    uint32_t key;               /* PC | Thumb bit */
    uint32_t rx_off;
    uint16_t count;
    uint8_t  state;             /* 0 empty, 1 counting, 2 compiled, 3 reject */
    uint8_t  hits;
} JitEntry;

typedef struct {
    Jit code;
    uint8_t *rw, *rx;
    uint32_t used;
    JitEntry *entry;
    int ready;
} JitContext;

typedef struct {
    uint32_t code[JIT_MAX_A64];
    uint32_t n;
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
    e_mov32(a, 0, retired);
    emit(a, 0xD65F03C0u);                          /* RET */
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
    switch (op) {
    case 0x0: base = setflags ? 0x6A000000u : 0x0A000000u; break; /* AND/S */
    case 0x1: base = 0x4A000000u; break;                         /* EOR */
    case 0x2: base = setflags ? 0x6B000000u : 0x4B000000u; break;/* SUB/S */
    case 0x4: base = setflags ? 0x2B000000u : 0x0B000000u; break;/* ADD/S */
    case 0xC: base = 0x2A000000u; break;                         /* ORR */
    case 0xE: base = 0x0A200000u; break;                         /* BIC */
    default: return 0;
    }
    e_rr(a, base, writes ? 9u : 31u, 9, 10);
    if (setflags) {
        if (op == 2 || op == 4)
            e_merge_nzcv(a, CPSR_N | CPSR_Z | CPSR_C | CPSR_V);
        else
            e_merge_nzcv(a, CPSR_N | CPSR_Z);
    }
    if (writes)
        e_store_r(a, 9, rd);
    return 1;
}

static int compile_arm(Guest *g, A64 *a, uint32_t start, uint32_t until,
                       uint16_t *out_count) {
    uint32_t pc = start, n = 0;
    while (n < JIT_MAX_GUEST && a->n + 40 < JIT_MAX_A64) {
        uint32_t insn, op, S, rn, rd;
        if (pc == (until & ~1u) || has_hook(g, pc) ||
            !guest_ld32(&g->mem, pc, &insn) || (insn >> 28) != 0xEu)
            break;

        if ((insn & 0x0E000000u) == 0x0A000000u) {       /* B / BL */
            int32_t off = (int32_t)(insn << 8) >> 6;
            if (insn & 0x01000000u) {
                e_mov32(a, 9, pc + 4u);
                e_store_r(a, 9, GUEST_LR);
            }
            e_set_pc(a, pc + 8u + (uint32_t)off);
            n++;
            e_return(a, n);
            *out_count = (uint16_t)n;
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
    e_return(a, n);
    *out_count = (uint16_t)n;
    return 1;
}

static int compile_thumb(Guest *g, A64 *a, uint32_t start, uint32_t until,
                         uint16_t *out_count) {
    uint32_t pc = start, n = 0;
    while (n < JIT_MAX_GUEST && a->n + 40 < JIT_MAX_A64) {
        uint32_t hw;
        if (pc == (until & ~1u) || has_hook(g, pc) ||
            !guest_ld16(&g->mem, pc, &hw))
            break;

        if ((hw & 0xF800u) >= 0xE800u) {                 /* selected T32 */
            uint32_t hw2, rn, rd, imm12, kind;
            if (!guest_ld16(&g->mem, pc + 2u, &hw2)) break;
            if ((hw & 0xFA00u) != 0xF200u || (hw2 & 0x8000u)) break;
            rn = hw & 0xFu; rd = (hw2 >> 8) & 0xFu;
            imm12 = (((hw >> 10) & 1u) << 11) |
                    (((hw2 >> 12) & 7u) << 8) | (hw2 & 0xFFu);
            kind = hw & 0xFBF0u;
            if (rd == GUEST_PC) break;
            if (kind == 0xF240u) {                       /* MOVW */
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
        } else if ((hw & 0xF800u) == 0xE000u) {          /* B */
            int32_t off = (int32_t)((hw & 0x7FFu) << 21) >> 20;
            e_set_pc(a, pc + 4u + (uint32_t)off);
            n++; e_return(a, n); *out_count = (uint16_t)n; return 1;
        } else break;
        pc += 2; n++;
    }
    if (!n) return 0;
    e_set_pc(a, pc); e_return(a, n); *out_count = (uint16_t)n;
    return 1;
}

static JitEntry *lookup(JitContext *j, uint32_t key) {
    uint32_t i = (key * 2654435761u) & (JIT_CACHE_SLOTS - 1u), n;
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
    memset(&a, 0, sizeof a);
    if (g->cpu.itstate || !(guest_is_thumb(&g->cpu)
          ? compile_thumb(g, &a, g->cpu.r[GUEST_PC], until, &count)
          : compile_arm(g, &a, g->cpu.r[GUEST_PC], until, &count))) {
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
    j->used = off + bytes;
    g->jit_blocks++;
    if (g->jit_blocks == 1 || g->jit_blocks == 100 ||
        (g->jit_blocks % 1000u) == 0)
        printf("  [jit  ] %u blocks, %u KB code\n",
               g->jit_blocks, j->used >> 10);
    return 1;
}

int guest_jit_init(Guest *g) {
    JitContext *j;
    Result rc;
    if (g->jit) return 1;
    j = (JitContext *)calloc(1, sizeof *j);
    if (!j) return 0;
    j->entry = (JitEntry *)calloc(JIT_CACHE_SLOTS, sizeof *j->entry);
    if (!j->entry) { free(j); return 0; }
    rc = jitCreate(&j->code, JIT_CODE_SIZE);
    if (R_FAILED(rc)) {
        printf("  [jit  ] unavailable (jitCreate=%08x), interpreter only\n", rc);
        free(j->entry); free(j); return 0;
    }
    j->rw = (uint8_t *)jitGetRwAddr(&j->code);
    j->rx = (uint8_t *)jitGetRxAddr(&j->code);
    j->ready = 1;
    g->jit = j;
    printf("  [jit  ] AArch64 cache ready: %u MB, type %d\n",
           JIT_CODE_SIZE >> 20, (int)j->code.type);
    return 1;
}

void guest_jit_close(Guest *g) {
    JitContext *j = (JitContext *)g->jit;
    if (!j) return;
    jitClose(&j->code);
    free(j->entry); free(j); g->jit = NULL;
}

int guest_jit_try_run(Guest *g, uint32_t until, uint64_t remaining,
                      uint32_t *retired) {
    JitContext *j;
    JitEntry *e;
    uint32_t key, n;
    JitBlockFn fn;
    *retired = 0;
    if (remaining < 2 || g->cpu.itstate || guest_is_stub(g->cpu.r[GUEST_PC]))
        return 0;
    if (!g->jit && !guest_jit_init(g)) return 0;
    j = (JitContext *)g->jit;
    key = g->cpu.r[GUEST_PC] | (guest_is_thumb(&g->cpu) ? 1u : 0u);
    e = lookup(j, key);
    if (!e) return 0;
    if (!e->state) { e->key = key; e->state = 1; e->hits = 1; return 0; }
    if (e->state == 3) return 0;
    if (e->state == 1) {
        if (++e->hits < JIT_HOT_COUNT) return 0;
        if (!compile_block(g, j, e, until)) return 0;
    }
    if (remaining < e->count) return 0;
    memcpy(&fn, &(void *){j->rx + e->rx_off}, sizeof fn);
    n = fn(g);
    if (n != e->count) return 0; /* generated leaf always returns its count */
    *retired = n;
    g->jit_executed += n;
    return 1;
}

#endif
