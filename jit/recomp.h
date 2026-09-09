/* recomp.h -- statically recompiled guest functions.
 *
 * The spike for whole-function ahead-of-time translation, as opposed to the
 * basic-block JIT in jit.c. The bet is that translating a guest function into
 * C and letting the host compiler have it beats both the interpreter and the
 * JIT, because GCC does for free the two things jit.c never got to: it keeps
 * guest registers in host registers across the whole function, and it deletes
 * every flag computation nothing goes on to read.
 *
 * Everything in recomp.c is written the way a GENERATOR would emit it, not the
 * way a person would write it by hand. That is the point: the hand-translated
 * functions here are a template for the emitter, so what is measured is what
 * the emitter would produce. Resist tidying them into idiomatic C.
 *
 * ---- the contract ------------------------------------------------------
 *
 * A RecompFn returns 1 when it ran the whole function and `c` holds the
 * result, and 0 when it DECLINED -- an unmapped guest address, or an encoding
 * the translation does not cover. On a decline `c` must be exactly as it was
 * on entry, so the caller can fall back to the interpreter at the same PC with
 * nothing to undo.
 *
 * That is what makes partial coverage safe, and it is the same invariant jit.c
 * already runs on: coverage is an optimisation property, never a correctness
 * one. It is enforced by discipline rather than by the type system -- guest
 * registers live in LOCALS for the body of a translated function and are
 * committed to `c` only on the paths that return 1. A translated function that
 * writes through `c` early and then declines would corrupt the run silently.
 *
 * PC is not the function's business. Entry is via the hook mechanism in
 * guest_run, which branches to LR once the handler returns, exactly as it does
 * for the hand-written native hooks. A translated function that has to make
 * its own control-flow decision about where to return has outgrown this spike.
 *
 * ---- flags -------------------------------------------------------------
 *
 * Flags are four separate locals, merged into CPSR only where state is
 * committed. Nothing reads them at a normal function boundary -- the ARM
 * procedure call standard makes them volatile across a call -- but the
 * differential harness compares CPSR, so a translation that got them wrong
 * would show up as a divergence rather than as a bug six frames later. Keeping
 * them exact here is what makes verification strict.
 *
 * The scaling decision this defers: at some function count, reproducing flags
 * that no guest instruction ever reads becomes the thing standing between the
 * emitter and its whole advantage. The answer then is to declare CPSR
 * don't-care at function boundaries and teach the verifier to skip it -- which
 * is correct by the PCS, and which should be done deliberately and once,
 * not discovered by a translation that quietly disagrees.
 */
#ifndef RECOMP_H
#define RECOMP_H

#include "guest.h"

/* Returns the number of guest instructions RETIRED, or 0 for a decline
 * (`g->cpu` untouched, the caller falls back to the interpreter).
 *
 * A count rather than a flag because the instruction counter is the axis
 * every cross-run comparison hangs off, and a hook that takes over a call
 * gets exactly one `g->executed++` from guest_run however many guest
 * instructions it stood in for. With 12M calls a window that undercounts
 * by ten each is 120M instructions of pure fiction -- and it only appears
 * when the translation is ON, which is precisely when a measurement is
 * trying to compare against OFF. The count must therefore be per-PATH:
 * a function that exits early retired fewer, and returning its maximum
 * would trade an undercount for an overcount.
 *
 * Count what the INTERPRETER would have retired, which is every
 * instruction stepped -- an ARM instruction whose condition fails is
 * still fetched, still counted, and must be counted here too.
 *
 * (The hand-written native hooks have the same undercount and are left
 * alone: they are present identically in both arms of any ablation, so
 * they shift the absolute number without biasing the comparison.)
 *
 * Takes the whole Guest rather than the CPU and memory, because a
 * translated function with outbound calls needs to re-enter the
 * interpreter -- see recomp_call below. The leaf that started this spike
 * needed neither, but every function large enough to matter has calls in
 * it, so the narrower signature was never going to survive. */
typedef int (*RecompFn)(Guest *g);

/* Call a guest function from translated code and come back.
 *
 * `target` is the callee with its Thumb bit, `ret` is what BL would have
 * written to LR -- the guest address just past the call, Thumb bit and
 * all. Both are what the ORIGINAL instruction would have used, so the
 * callee sees a register file identical to the one it would have seen
 * under the interpreter.
 *
 * Using the real return address as both LR and the stopping point, rather
 * than a landing pad, is what makes that true. guest_call uses a pad in
 * the stub page, which is fine when nothing observes LR, but a callee here
 * may well spill it -- and a spilled pad address is a divergence that
 * would surface much later as a wild branch.
 *
 * The caller must commit its register state to g->cpu BEFORE calling and
 * reload afterwards: the callee can read and write any of it, and the ARM
 * PCS only promises r4-r11 come back intact.
 *
 * Returns 1 when the callee returned normally. 0 means it faulted or ran
 * past its budget, and the translated function must then decline -- except
 * that by this point it has already committed state, so it cannot. A
 * translated function that has made an outbound call is past the point of
 * no return, which is exactly why the budget is generous and why a 0 here
 * is a bug to investigate rather than a condition to handle.
 *
 * Recursion caveat: if the callee re-enters this same call site and
 * returns to the same address, the run stops one level early. guest_call
 * has the same exposure. Nothing translated so far is recursive. */
int recomp_call(Guest *g, uint32_t target, uint32_t ret);

typedef struct {
    uint32_t    rva;      /* entry point, as an RVA off load_base */
    RecompFn    fn;
    const char *name;     /* for the report; what the function does, not its address */
} RecompEntry;

extern const RecompEntry g_recomp[];
extern const unsigned    g_recomp_count;

#endif /* RECOMP_H */
