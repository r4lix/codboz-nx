/* recomp.c -- statically recompiled guest functions. See recomp.h.
 *
 * Written the way an emitter would emit, deliberately: fixed local names for
 * the flags, one C statement per guest instruction, the original mnemonic in a
 * trailing comment, and no restructuring of the control flow into something a
 * person would have preferred. What is measured here has to be what a
 * generator would produce, or the measurement answers the wrong question.
 */
#include "recomp.h"

/* The guest's ctype table, resolved at startup in main.c, and the tally
 * the [fast ] report prints. Shared so a translation can apply tolower
 * inline instead of calling the hook once per character. */
extern uint32_t g_ctype_got;
extern uint32_t g_ctype_hits[2];

/* ---------------------------------------------------------------- flags ---
 *
 * The emitter's calling convention with itself: every translated function
 * declares nf/zf/cf/vf as locals and these macros write them by name. Fixed
 * names are what let the macros stay this small; a generator has no trouble
 * emitting the declaration, and GCC deletes whichever of the four the rest of
 * the function never reads.
 *
 * Subtraction is done the way the interpreter does it -- a + ~b + 1 widened to
 * 64 bits -- so carry comes out of bit 32 rather than from a comparison that
 * would have to special-case b == 0. */
#define RC_FLAGS_SUB(a, b)                                                     \
    do {                                                                       \
        uint32_t _a = (a), _b = (b);                                           \
        uint64_t _w = (uint64_t)_a + (uint64_t)(uint32_t)(~_b) + 1ull;         \
        uint32_t _r = (uint32_t)_w;                                            \
        nf = _r >> 31;                                                         \
        zf = (_r == 0u);                                                       \
        cf = (uint32_t)(_w >> 32);                                             \
        vf = (((_a ^ _b) & (_a ^ _r)) >> 31);                                  \
    } while (0)

#define RC_EQ (zf)
#define RC_NE (!zf)
#define RC_LE (zf || (nf != vf))

/* Merge the flag locals back into CPSR. Only on paths that return 1: a
 * declining path must leave the guest exactly as it found it. */
#define RC_COMMIT_FLAGS(c)                                                     \
    ((c)->cpsr = ((c)->cpsr & ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V))            \
                 | (nf ? CPSR_N : 0u) | (zf ? CPSR_Z : 0u)                     \
                 | (cf ? CPSR_C : 0u) | (vf ? CPSR_V : 0u))

/* Re-entry into the interpreter for an outbound call. See recomp.h.
 *
 * The interworking rule is replicated from branch_interworking in
 * interp.c rather than shared, because that one is static to the
 * interpreter. It masks bit 0 only -- NOT bit 1 for the ARM case -- and
 * a translation that helpfully aligned to 4 would branch somewhere the
 * interpreter would not. */
int recomp_call(Guest *g, uint32_t target, uint32_t ret) {
    if (target & 1u)
        g->cpu.cpsr |= CPSR_T;
    else
        g->cpu.cpsr &= ~CPSR_T;
    g->cpu.r[15] = target & ~1u;
    g->cpu.r[GUEST_LR] = ret;
    return guest_run(g, ret, 20000000) == GUEST_HALTED;
}

/* ------------------------------------------------------- 0x0024ca44 -------
 *
 * Unaligned little-endian load of 1 to 4 bytes. r1 = source pointer,
 * r2 = count, result in r0:
 *
 *     r0 = b[0] | b[1]<<8 | b[2]<<16 | b[3]<<24, truncated to `count` bytes
 *
 * with the count compared SIGNED, so anything <= 1 -- zero and negatives
 * included -- reads exactly one byte.
 *
 * Thirteen instructions, no prologue, no stores, reached by BL and leaving
 * by BX LR. It is a leaf the compiler evidently outlined rather than
 * inlined.
 *
 * Retired counts, which are what this returns: 3 on the count<=1 exit, 7 on
 * the count==2 exit, and 13 otherwise. The last two predicated instructions
 * are counted even when count==3 makes their condition fail -- ARM steps
 * them regardless, and so does the interpreter.
 *
 * Chosen as the first translation for what it does to the JIT rather than for
 * its share of the profile. It is the top entry in the [jitb] blocker table:
 * two conditional returns and two predicated instructions mean it terminates
 * four basic blocks on its own, which is exactly the shape that holds mean
 * block length near three and stops the JIT ever amortising its dispatch. A
 * whole-function translation does not care -- the conditions become ordinary C
 * control flow and the whole thing is one host function with no dispatch at
 * all. That contrast is what the spike is trying to measure.
 *
 * It is NOT expected to move the frame rate. The blocker table ranks what ends
 * blocks; pcprof ranks what runs, and this address is not in its top twelve.
 * The number to read off this function is the per-call ratio, not fps.
 *
 * Register fidelity, which the vertex-transform hook learned the hard way:
 * the real function leaves r3 holding the LAST byte it loaded, and CPSR
 * holding the flags of the last CMP it executed -- both differ per path, and
 * both are compared by the differential. r1 and r2 it never touches. */
static int r_0024ca44(Guest *gu) {
    GuestCpu *c = &gu->cpu;
    GuestMem *m = &gu->mem;
    uint32_t r0, r1 = c->r[1], r2 = c->r[2], r3 = c->r[3];
    uint32_t nf, zf, cf, vf;

    uint32_t retired;

    RC_FLAGS_SUB(r2, 1u);                          /* cmp    r2, #1        */
    if (!guest_ld8(m, r1, &r0))                    /* ldrb   r0, [r1]      */
        return 0;
    if (RC_LE) {                                   /* bxle   lr            */
        retired = 3;
        goto done;
    }

    if (!guest_ld8(m, r1 + 1u, &r3))               /* ldrb   r3, [r1, #1]  */
        return 0;
    RC_FLAGS_SUB(r2, 2u);                          /* cmp    r2, #2        */
    r0 = r0 | (r3 << 8);                           /* orr    r0,r0,r3,lsl#8 */
    if (RC_EQ) {                                   /* bxeq   lr            */
        retired = 7;
        goto done;
    }

    if (!guest_ld8(m, r1 + 2u, &r3))               /* ldrb   r3, [r1, #2]  */
        return 0;
    RC_FLAGS_SUB(r2, 3u);                          /* cmp    r2, #3        */
    r0 = r0 | (r3 << 16);                          /* orr   r0,r0,r3,lsl#16 */
    if (RC_NE) {                                   /* ...ne                */
        if (!guest_ld8(m, r1 + 3u, &r3))           /* ldrbne r3, [r1, #3]  */
            return 0;
        r0 = r0 | (r3 << 24);                      /* orrne r0,r0,r3,lsl#24 */
    }
    retired = 13;

done:                                              /* bx     lr            */
    c->r[0] = r0;
    c->r[3] = r3;
    RC_COMMIT_FLAGS(c);
    return (int)retired;
}

/* RVA 0x000ba13c was translated here and REMOVED. Two lessons, both paid for:
 *
 * It was the wrong function. The profiler put 82%% of a 6%% range inside the
 * 16-byte bucket at 0x0ba160, and 0x0ba160 lies inside the sibling that starts
 * at 0x0ba154 -- 0x0ba13c ends at its pop two bytes earlier. Translated, it
 * reported ONE call per frame. That is the range-versus-function trap for the
 * third time in this port, after 0x006450 and 0x062f60, and this time the
 * per-bucket data that would have prevented it was already on screen.
 *
 * It was also incorrect. Even at one call per frame the game diverged: the
 * affine-compose tally decayed 264871 -> 243699 -> 144117 across a single run
 * while frames and instruction budget held, which is a scene quietly emptying
 * out because a dispatch-to-listeners walk stopped reaching all its listeners.
 * A translation can return to the right place, never fault, and still be
 * wrong -- which is what the page-granular verification is for, and it had not
 * been run on this function.
 *
 * The real target is 0x0ba154: same list walk, but it calls vtable slot 6 per
 * element, compares the result against its first argument, and calls slot 9
 * with the second argument on a match. Two virtual calls per matching element.
 * Before translating it, settle whether recomp_call is cheap enough -- each
 * outbound call currently pays a full nested guest_run entry, and for a loop
 * that calls once or twice per element that may cost more than the seven
 * interpreted instructions it replaces. */

/* ------------------------------------------------------- 0x000ba154 -------
 *
 * The list walk the profiler actually pointed at: 82% of a 6% range sits in
 * the 16-byte bucket at 0x0ba160, which is this function's loop head. The
 * neighbour at 0x0ba13c, translated first by mistake, ran once a frame.
 *
 * Dispatch-to-matching-listeners. r0 = container, r1 = the value to match,
 * r2 = the payload:
 *
 *     r6 = r1;  r7 = r2;  r5 = [r0+12];  r4 = [r5]
 *   loop:
 *     if (r4 == r5) done
 *     r0 = [r4+8]                      the element's object
 *     call [[r0]+24](r0)               vtable slot 6 -- a getter
 *     if (r0 == r6) {                  matches what the caller asked for
 *         r0 = [r4+8]
 *         call [[r0]+36](r0, r7)       vtable slot 9 -- the payload
 *     }
 *     r4 = [r4]                        next
 *
 * So one virtual call per element and two per match. That call density is the
 * point of translating it: if recomp_call is cheap enough that replacing ten
 * to fifteen interpreted instructions per element beats the re-entry cost,
 * translation pays on virtual-dispatch code, and virtual dispatch is most of
 * what is left in the profile. If it does not, hot-spot translation is
 * finished as a strategy and only broad automatic coverage remains.
 *
 * LR IS SAVED AND RESTORED. recomp_call clobbers it exactly as a real BLX
 * does, and a translated function returns through the hook branching to
 * c->r[LR]. Without this the first call makes the function return into its own
 * middle -- which is precisely how the previous attempt walked off into
 * unmapped memory. r3-r7 come back at their entry values because the real
 * function pops them; r0-r2 are left as the callees leave them.
 *
 * VERIFY BEFORE TRUSTING. The last translation returned to the right place,
 * never faulted, and still quietly emptied the scene. Run this under
 * recompverify.txt first and watch the divergence count, not the frame rate.
 */
static int r_0ba154(Guest *gu) {
    GuestCpu *c = &gu->cpu;
    GuestMem *m = &gu->mem;
    uint32_t base = c->r[15] - 0x000ba154u;
    uint32_t ret1 = base + 0x000ba16au + 1u;   /* after the first blx  */
    uint32_t ret2 = base + 0x000ba178u + 1u;   /* after the second blx */
    uint32_t s_r3 = c->r[3], s_r4 = c->r[4], s_r5 = c->r[5];
    uint32_t s_r6 = c->r[6], s_r7 = c->r[7], s_lr = c->r[GUEST_LR];
    uint32_t r6 = c->r[1], r7 = c->r[2], r4, r5;
    uint32_t sp = c->r[GUEST_SP];
    uint32_t retired = 5u;                     /* push, mov, ldr, mov, ldr */

    /* Emulate the frame, do not merely save the registers in locals.
     *
     * The callees run with the guest SP as they find it. Keeping the saved
     * registers in C and leaving SP alone means every callee builds its frame
     * 24 bytes higher than it really would, and page verification catches that
     * immediately as a stack address that differs between the two arms -- which
     * is how this was found, on the first run of the checker.
     *
     * push {r3,r4,r5,r6,r7,lr}: SP drops 24, lowest register at the lowest
     * address. Any translated function with a prologue has to do this. */
    sp -= 24u;
    if (!guest_st32(m, sp +  0u, s_r3) || !guest_st32(m, sp +  4u, s_r4) ||
        !guest_st32(m, sp +  8u, s_r5) || !guest_st32(m, sp + 12u, s_r6) ||
        !guest_st32(m, sp + 16u, s_r7) || !guest_st32(m, sp + 20u, s_lr))
        return 0;
    c->r[GUEST_SP] = sp;

    if (!guest_ld32(m, c->r[0] + 12u, &r5))
        return 0;
    if (!guest_ld32(m, r5, &r4))
        return 0;

    for (;;) {
        uint32_t obj, vt, fn;

        retired += 2u;                          /* cmp r4, r5 / beq */
        if (r4 == r5)
            break;

        if (!guest_ld32(m, r4 + 8u, &obj) ||
            !guest_ld32(m, obj, &vt) ||
            !guest_ld32(m, vt + 24u, &fn))
            break;                              /* committed: cannot decline */
        retired += 4u;                          /* three loads and the blx */

        c->r[0] = obj;
        c->r[3] = fn;
        c->r[4] = r4;
        c->r[5] = r5;
        c->r[6] = r6;
        c->r[7] = r7;
        if (!recomp_call(gu, fn, ret1))
            break;
        r4 = c->r[4];                           /* reload: trust the machine */
        r5 = c->r[5];
        r6 = c->r[6];
        r7 = c->r[7];

        retired += 2u;                          /* cmp r0, r6 / bne */
        if (c->r[0] == r6) {
            if (!guest_ld32(m, r4 + 8u, &obj) ||
                !guest_ld32(m, obj, &vt) ||
                !guest_ld32(m, vt + 36u, &fn))
                break;
            retired += 5u;                      /* ldr, mov, ldr, ldr, blx */
            c->r[0] = obj;
            c->r[1] = r7;
            c->r[3] = fn;
            c->r[4] = r4;
            c->r[5] = r5;
            c->r[6] = r6;
            c->r[7] = r7;
            if (!recomp_call(gu, fn, ret2))
                break;
            r4 = c->r[4];
            r5 = c->r[5];
            r6 = c->r[6];
            r7 = c->r[7];
        }

        if (!guest_ld32(m, r4, &r4))
            break;
        retired += 2u;                          /* ldr r4,[r4] and the b */
    }

    /* pop {r3,r4,r5,r6,r7,pc}: read the frame back rather than trusting the
     * locals, because a callee that overwrote it is a real event the
     * interpreter would reproduce and the translation must too. */
    {
        uint32_t v;
        if (guest_ld32(m, sp +  0u, &v)) c->r[3] = v;
        if (guest_ld32(m, sp +  4u, &v)) c->r[4] = v;
        if (guest_ld32(m, sp +  8u, &v)) c->r[5] = v;
        if (guest_ld32(m, sp + 12u, &v)) c->r[6] = v;
        if (guest_ld32(m, sp + 16u, &v)) c->r[7] = v;
        if (guest_ld32(m, sp + 20u, &v)) c->r[GUEST_LR] = v;
    }
    c->r[GUEST_SP] = sp + 24u;
    retired += 1u;
    return (int)retired;
}

/* ------------------------------------------------------- 0x0007c304 -------
 *
 * Case-insensitive djb2 string hash, and the cleanest target left:
 *
 *     h = 5381
 *     for p from end down to start:  h = (h * 33) ^ tolower(*--p)
 *
 * 5381 is djb2's seed and `add r6, r0, r0, lsl #5` is its multiply by 33.
 * The string is the same small-string-optimised 16-byte descriptor the rest
 * of this image uses: byte 15 holds 255 for a heap string, whose data pointer
 * lives at +4, and otherwise the characters sit inline at the descriptor.
 *
 * Worth translating for what it is as much as its share: a PURE function, no
 * stores anywhere, so verification is the register-only comparison that ran
 * clean over 46M calls on read_le_1_4 -- not the page-rollback path that
 * needed three attempts. It is the opposite of the hash-map lookup at
 * 0x0631b4, which carries the larger share of the tolower traffic and was
 * left alone precisely because reimplementing a probe sequence risks lookups
 * that fail silently rather than loudly.
 *
 * The length comes from an outbound call rather than being recomputed here.
 * The SSO layout makes it guessable -- 15 minus byte 15 for inline, a field
 * for heap -- but guessing the length of every string in the game to save one
 * call per hash is a poor trade, and recomp_call is paid once per invocation
 * rather than once per character.
 *
 * tolower is replicated inline instead of called. The guest's is table-driven
 * through the GOT, not ASCII, so this reads the same class byte the hook does
 * and applies the same +32; anything the table does not mark stays untouched.
 * That removes a hook dispatch and three dependent guest loads per character,
 * which is where most of the cost of this loop actually is. */
static int r_07c304(Guest *gu) {
    GuestCpu *c = &gu->cpu;
    GuestMem *m = &gu->mem;
    uint32_t base = c->r[15] - 0x0007c304u;
    uint32_t ret  = base + 0x0007c30cu + 1u;   /* after the length call */
    uint32_t s_r4 = c->r[4], s_r5 = c->r[5], s_r6 = c->r[6];
    uint32_t s_lr = c->r[GUEST_LR];
    uint32_t desc = c->r[0], data, len, tbl_p = 0, tbl = 0;
    uint32_t h = 5381u, marker;
    uint32_t retired = 3u;                     /* push, mov, bl */

    /* The ctype table has to be resolvable, or tolower's behaviour is not
     * reproducible and this must decline rather than guess. */
    if (!g_ctype_got ||
        !guest_ld32(m, g_ctype_got, &tbl_p) ||
        !guest_ld32(m, tbl_p, &tbl))
        return 0;

    c->r[0] = desc;
    if (!recomp_call(gu, base + 0x0005b60au + 1u, ret))
        return 0;                              /* nothing committed yet */
    len = c->r[0];

    if (!guest_ld8(m, desc + 15u, &marker))
        return 0;
    data = (marker == 255u) ? 0u : desc;
    if (marker == 255u && !guest_ld32(m, desc + 4u, &data))
        return 0;
    retired += 6u;                             /* ldrb, cmp, it, ldreq, adds, movw */

    {
        uint32_t p = data + len;
        while (p > data) {
            uint32_t ch = 0, cls = 0;
            uint32_t mul = h + (h << 5);       /* h * 33, before the load */
            if (!guest_ld8(m, p - 1u, &ch))
                break;
            p--;
            /* tolower, exactly as ctype_convert does it: class bits 1:0 == 1
             * means upper-case in this table, and only then does +32 apply. */
            if (guest_ld8(m, tbl + ch + 1u, &cls) && (cls & 3u) == 1u)
                ch += 32u;
            g_ctype_hits[0]++;                 /* keep the [fast ] tally true */
            h = ch ^ mul;
            retired += 9u;                     /* 8 in the loop, 1 for the hook */
        }
        retired += 3u;                         /* the subs/cmp/ble that exits */
    }

    c->r[0] = h;
    /* The loop leaves state behind that the caller can see, and r3 is not in
     * the push list so nothing restores it. Exit runs
     *     subs r3, r5, r4   ; remaining, which is 0 once p has reached data
     *     cmp  r3, #0
     *     ble  done
     * so r3 ends at 0 and the flags are those of the cmp: Z and C set, N and V
     * clear. Verification caught r3 immediately; CPSR would have been the next
     * report, since the same cmp sets it. */
    c->r[3] = 0;
    c->cpsr = (c->cpsr & ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V)) | CPSR_Z | CPSR_C;
    c->r[4] = s_r4;                            /* pop {r4, r5, r6, pc} */
    c->r[5] = s_r5;
    c->r[6] = s_r6;
    c->r[GUEST_LR] = s_lr;
    retired += 1u;
    return (int)retired;
}

/* MEASURED OUTCOMES, so nobody re-runs these experiments.
 *
 *   read_le_1_4      13 instructions, NO outbound calls. Verified over 46M
 *                    calls with zero divergences. A WIN: -27.8% and -45.8% on
 *                    the two loading windows, -1.9% in gameplay.
 *
 *   str_hash_ci      pure, no stores, one outbound call for the string length.
 *                    Correct -- the affine tally matched the baseline exactly
 *                    and it cleared the point three earlier attempts died at.
 *                    A LOSS: 208640 ms against a 202354-203244 baseline,
 *                    frame-matched. ~2.8% slower.
 *
 *   listener_dispatch  never proven. Its checks corrupted state before the
 *                    harness bugs were found; it has not been re-run since.
 *                    Do not enable without verifying it first.
 *
 * The rule those three establish: A TRANSLATION PAYS ONLY IF IT MAKES NO
 * OUTBOUND CALLS. recomp_call re-enters guest_run, and at a few thousand
 * invocations a frame that costs more than the interpreted instructions it
 * removes -- even at one call per invocation, let alone one per element. Every
 * remaining hot spot in the profile is virtual dispatch or a container lookup,
 * so all of them call out per element. Making recomp_call roughly free, by
 * dispatching the callee inline rather than re-entering the interpreter, is
 * the prerequisite for any of this being worth extending.
 *
 * All three are off unless recomp_mask selects them. */

/* The table the harness installs from. Adding the next translated function is
 * one line here plus the function itself -- which is the property the emitter
 * will need, so the spike is built with it from the start.
 *
 * Next candidate is RVA 0x006450: 1360 bytes, a 156-byte frame, outbound
 * calls, and 6% of every guest instruction. It was written off as un-hookable,
 * and it is -- by hand. It is not un-TRANSLATABLE, and it is the function that
 * would show whether this approach reaches the parts native hooks cannot. */
const RecompEntry g_recomp[] = {
    { 0x0024ca44u, r_0024ca44, "read_le_1_4" },
    { 0x0007c304u, r_07c304,   "str_hash_ci" },
    { 0x000ba154u, r_0ba154,   "listener_dispatch" },
};

const unsigned g_recomp_count = (unsigned)(sizeof g_recomp / sizeof g_recomp[0]);
