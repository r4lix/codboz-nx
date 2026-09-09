/* dynarmic_glue.h -- run the guest on Dynarmic instead of the interpreter.
 *
 * Same contract as guest_run, so a caller can be switched over one call site
 * at a time and the two can be compared against each other on the device.
 * Everything the interpreter observes -- g->cpu, g->mem, g->executed, the HLE
 * stub page, the native hooks -- is observed identically here; what changes is
 * only who executes the guest instructions in between.
 *
 * Returns 0 from dyn_init when Dynarmic is unavailable, which on Horizon means
 * executable memory could not be obtained. That is not a fatal condition: it
 * depends on how the homebrew was launched, and the interpreter is still there.
 * Callers must check and fall back.
 */
#ifndef DYNARMIC_GLUE_H
#define DYNARMIC_GLUE_H

#include "guest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the JIT and its page table for the regions currently registered.
 * Call after every guest_mem_add; returns non-zero on success. */
int  dyn_init(Guest *g, unsigned code_cache_mb);
void dyn_close(Guest *g);

/* guest_run's contract exactly: run until `limit` instructions retire, PC
 * reaches `until` (GUEST_HALTED), or a fault. */
GuestStatus dyn_run(Guest *g, uint32_t until, uint64_t limit);

/* Drop translations covering [addr, addr+len). Needed only if guest code is
 * ever rewritten; the loader does that once, before the JIT exists. */
void dyn_invalidate(Guest *g, uint32_t addr, uint32_t len);

/* Dump the JIT's state to the log. Safe to call from the control socket's
 * thread while the guest thread is wedged -- that is the point of it. */
void dyn_diag(void);

/* One line for the log: whether it is live, and what it has done. */
void dyn_report(Guest *g);

#ifdef __cplusplus
}
#endif

#endif /* DYNARMIC_GLUE_H */
