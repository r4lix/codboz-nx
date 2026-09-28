/* dynarmic_glue.cpp -- the guest, executed by Dynarmic.
 *
 * The interpreter in interp.c is the reference: it has been checked against
 * Unicorn to 2.7 billion instructions, and everything the game depends on --
 * the HLE stub page, the native hooks, the fastmem window -- was built around
 * its behaviour. So this file changes exactly one thing, who executes guest
 * instructions, and reproduces the rest verbatim rather than reinventing it.
 * Where a comment here says "as guest_run does", that is a promise about
 * observable behaviour and the two need to be read together.
 *
 * ---- what dynarmic already gives us --------------------------------------
 *
 * Two pieces of dynarmic's design do most of the work:
 *
 * The page table means memory accesses do not have to go through callbacks.
 * Configured with absolute_offset_page_table false, generated code computes
 * page_table[addr >> 12] + (addr & 0xFFF) inline -- one load and an add, which
 * is what the interpreter's fastmem window costs and without needing the
 * window to exist. It matters because the alternative, a call per guest load,
 * would give back most of what a JIT is for; and because dynarmic's OTHER fast
 * path, fastmem, needs a segfault handler to patch faulting accesses, and
 * Horizon has no signals to build one from.
 *
 * PreCodeReadHook lets a translation be intercepted at a guest address before
 * the instruction there is read. That is precisely the shape of both HLE
 * mechanisms this port already has -- the import stub page, where no
 * instruction exists at all, and the native function hooks -- so neither needs
 * the guest image patched, and neither costs anything at run time: the
 * decision is made once when a block is translated, not on every execution.
 *
 * ---- and what it does not ------------------------------------------------
 *
 * Dynarmic owns its register file. Every HLE handler in this port takes a
 * GuestCpu*, so a handler call means copying registers out and back. That is
 * ~80 words twice per HLE call, and HLE calls number in the thousands per
 * frame rather than the millions, so it is not worth reshaping every handler
 * to avoid. If a profile ever says otherwise, the fix is to point the handlers
 * at dynarmic's array directly, not to make the copy cleverer.
 */

/* The whole file is opt-in. SOURCES globs this directory, so without the
 * guard a build with DYNARMIC unset would still try to compile against
 * headers it has no include path for. The stubs below keep dyn_init's
 * contract -- it returns 0 and the caller falls back to the interpreter,
 * which is the same path taken when executable memory cannot be had. */
#ifdef BOZ_DYNARMIC

/* libnx first, and BIT undefined immediately: it is a macro here and an
 * instruction mnemonic in oaknut, and whichever header comes second
 * loses. See dynarmic_patches/04-fixups.py for the same collision. */
#include <switch.h>
#undef BIT

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>

#include "dynarmic_glue.h"

namespace {

using VAddr = Dynarmic::A32::VAddr;

/* Supervisor-call tags.
 *
 * The interception emits IR directly rather than planting an SVC instruction
 * in the guest image, so this number never has to fit an ARM imm24 or a Thumb
 * imm8 -- it is a full 32-bit value we choose. Which is what makes one tag
 * space work for 768 import slots and every native hook at once; encoding them
 * into a Thumb SVC would have capped the whole scheme at 256.
 *
 * The base is high and arbitrary so that a real SVC in guest code, if this
 * image ever contained one, is unlikely to collide. It should not: a Marmalade
 * app reaches the OS through its import table, which is the stub page, and
 * never through a syscall. An unrecognised tag is reported rather than
 * silently treated as a hook. */
enum : std::uint32_t {
    SvcBase = 0x00A00000u,
    SvcHalt = SvcBase,          /* PC reached the caller's `until` */
    SvcHook = SvcBase + 0x1000u,/* + index into Guest::hook */
    SvcStub = SvcBase + 0x8000u,/* + import slot index */
    SvcNone = 0xFFFFFFFFu,
};

constexpr std::size_t kPageBits = Dynarmic::A32::UserConfig::PAGE_BITS;
constexpr std::size_t kPageSize = std::size_t(1) << kPageBits;
using PageTable = std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;

/* As branch_interworking in interp.c: bit 0 of the target selects the
 * instruction set and is not part of the address. */
void BranchInterworking(GuestCpu& c, std::uint32_t target) {
    if (target & 1u)
        c.cpsr |= CPSR_T;
    else
        c.cpsr &= ~CPSR_T;
    c.r[15] = target & ~1u;
}

/* ITSTATE lives in two separate fields of a real CPSR -- IT[7:2] at bits
 * 15:10 and IT[1:0] at bits 26:25 -- which is how dynarmic carries it. This
 * port keeps it unpacked in its own word because the interpreter advances it
 * explicitly, so it has to be folded in on the way down and stripped back out
 * on the way up. Getting this wrong would not fault; it would silently
 * mispredicate the instructions after an IT block, which is the kind of bug
 * that surfaces as one wrong pixel an hour later. */
constexpr std::uint32_t kItMask = 0x0600FC00u;

std::uint32_t PackCpsr(const GuestCpu& c) {
    std::uint32_t v = (c.cpsr & ~kItMask) | 0x10u; /* User mode */
    v |= (c.itstate & 0xFCu) << 8;
    v |= (c.itstate & 0x03u) << 25;
    return v;
}

void UnpackCpsr(GuestCpu& c, std::uint32_t v) {
    c.itstate = ((v >> 8) & 0xFCu) | ((v >> 25) & 0x03u);
    c.cpsr = v & ~kItMask;
}

class BozEnv final : public Dynarmic::A32::UserCallbacks {
public:
    Guest* g = nullptr;
    Dynarmic::A32::Jit* jit = nullptr;
    std::unique_ptr<PageTable> page_table;

    std::uint32_t until = 0xFFFFFFFFu;
    std::uint64_t budget = 0;
    std::uint64_t used = 0;
    GuestStatus status = GUEST_OK;
    bool stop = false;      /* a tag or a fault asked to leave Run() */
    bool halted = false;    /* ...and it was because PC reached `until` */

    std::uint64_t hle_calls = 0;
    std::uint64_t hook_calls = 0;
    std::uint64_t unpredictable = 0;
    /* Instrumentation for a hang that happens INSIDE Run().
     *
     * The watchdog in dyn_run can only see stalls where Run() returns, and it
     * never fired -- so whatever is looping never gets back to the host, and
     * nothing outside these callbacks can observe it. The callbacks can: every
     * interception and every code fetch passes through here, so a count that
     * keeps climbing says which of the two is spinning, and one that stays put
     * says neither is and the guest is stuck inside a handler or a block.
     *
     * The first few are printed individually because the shape of the first
     * dispatches is usually enough to recognise a loop; after that it is one
     * line per 200k, which is nothing next to the millions of instructions a
     * second the thing is meant to be doing. */
    std::uint64_t svc_calls = 0;
    std::uint64_t fetches = 0;

    /* Where the time actually goes.
     *
     * Startup runs at roughly 22k guest instructions a second while the same
     * build later sustains millions, and three plausible explanations have now
     * been measured and killed: the W^X transitions cost 4 us a round trip,
     * the log emits five lines across the whole slow window, and the dispatch
     * ring shows no observe hooks at all. Guessing a fourth would be the same
     * mistake again.
     *
     * t_svc is time inside the interception callback -- the HLE handlers and
     * the register syncs around them. t_run is time inside Jit::Run(), which
     * CONTAINS t_svc, so the difference is everything dynarmic does on its own:
     * translating, optimising, allocating registers, emitting, and running the
     * result. One subtraction says which half to work on. */
    std::uint64_t t_svc = 0;
    std::uint64_t t_run = 0;
    std::uint64_t runs = 0;

    /* The last few interceptions, kept so the control socket can show them
     * while the guest thread is stuck.
     *
     * Printing every dispatch is too much and printing every 200,000 turned
     * out to be too little -- the hang lives inside a window of at most
     * 900,000 guest instructions, so a threshold picked for millions per
     * second never fires and says nothing either way. A ring costs one store
     * per dispatch and does not need the threshold guessed correctly, because
     * it is read on demand from the other thread rather than pushed. */
    struct Dispatch {
        std::uint32_t pc, lr, tag;
    };
    static constexpr unsigned kRing = 32;
    Dispatch ring[kRing] = {};
    unsigned ring_n = 0;

    /* Same 256-entry filter guest_run uses, for the same reason: without it
     * every translated instruction pays a linear scan of the hook table. Keyed
     * on (pc >> 1) so ARM and Thumb addresses spread across it evenly. A set
     * bit means "maybe", and the scan then confirms. */
    std::uint8_t hook_map[256] = {};
    std::uint32_t hook_map_for = 0xFFFFFFFFu;

    /* ---------------------------------------------------------- memory */

    /* An unmapped access has no return value that is safe to invent, so it
     * ends the run the way guest_run does: record the address, report
     * GUEST_FAULT_MEM, and stop. Returning zero and continuing would turn a
     * wild pointer into silently wrong output. */
    void Fault(VAddr addr) {
        if (status == GUEST_OK) {
            status = GUEST_FAULT_MEM;
            g->fault_addr = addr;
        }
        Stop();
    }

    void Stop() {
        stop = true;
        if (jit)
            jit->HaltExecution();
    }

    std::uint8_t MemoryRead8(VAddr a) override {
        std::uint32_t v = 0;
        if (!guest_ld8(&g->mem, a, &v)) Fault(a);
        return static_cast<std::uint8_t>(v);
    }
    std::uint16_t MemoryRead16(VAddr a) override {
        std::uint32_t v = 0;
        if (!guest_ld16(&g->mem, a, &v)) Fault(a);
        return static_cast<std::uint16_t>(v);
    }
    std::uint32_t MemoryRead32(VAddr a) override {
        std::uint32_t v = 0;
        if (!guest_ld32(&g->mem, a, &v)) Fault(a);
        return v;
    }
    /* Split rather than widened: guest.h deliberately has no 64-bit accessor,
     * because a span that crosses a region boundary must fault rather than
     * read past the end, and two checked halves are exactly that test. */
    std::uint64_t MemoryRead64(VAddr a) override {
        std::uint32_t lo = 0, hi = 0;
        if (!guest_ld32(&g->mem, a, &lo) || !guest_ld32(&g->mem, a + 4, &hi)) Fault(a);
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
    }

    void MemoryWrite8(VAddr a, std::uint8_t v) override {
        if (!guest_st8(&g->mem, a, v)) Fault(a);
    }
    void MemoryWrite16(VAddr a, std::uint16_t v) override {
        if (!guest_st16(&g->mem, a, v)) Fault(a);
    }
    void MemoryWrite32(VAddr a, std::uint32_t v) override {
        if (!guest_st32(&g->mem, a, v)) Fault(a);
    }
    void MemoryWrite64(VAddr a, std::uint64_t v) override {
        if (!guest_st32(&g->mem, a, static_cast<std::uint32_t>(v)) ||
            !guest_st32(&g->mem, a + 4, static_cast<std::uint32_t>(v >> 32)))
            Fault(a);
    }

    /* Instruction fetch, always 4-byte aligned by contract. Uses the fetch
     * accessor rather than the load one so it keeps its own region cache --
     * see GuestMem::icache for why sharing one costs a third of all fetches.
     * nullopt means unmapped, which dynarmic turns into a NoExecuteFault. */
    std::optional<std::uint32_t> MemoryReadCode(VAddr a) override {
        if ((++fetches % 200000ull) == 0ull)
            std::printf("  [dyn  ] %llu code fetches, latest %08x\n",
                        (unsigned long long)fetches, (unsigned)a);
        std::uint32_t v = 0;
        if (!guest_ifetch32(&g->mem, a, &v))
            return std::nullopt;
        return v;
    }

    /* ------------------------------------------------------- the hooks */

    void RefreshHookMap() {
        if (hook_map_for == g->hook_count)
            return;
        std::memset(hook_map, 0, sizeof hook_map);
        for (std::uint32_t i = 0; i < g->hook_count; i++)
            hook_map[((g->hook[i].addr & ~1u) >> 1) & 0xFFu] = 1;
        hook_map_for = g->hook_count;
    }

    std::uint32_t TagFor(std::uint32_t pc) const {
        if (guest_is_stub(pc))
            return SvcStub + guest_stub_index(pc);
        if ((pc & ~1u) == (until & ~1u))
            return SvcHalt;
        if (g->hook_count && hook_map[(pc >> 1) & 0xFFu]) {
            for (std::uint32_t i = 0; i < g->hook_count; i++) {
                if ((g->hook[i].addr & ~1u) != pc)
                    continue;
                /* Observing hooks are intercepted too. They were not, on
                 * the theory that they are all diagnostics, and that was
                 * wrong: fast_angle_normalize is registered this way and is
                 * load-bearing. It sits on `while (angle > 2pi) angle -= 2pi`
                 * and, when the angle is large, replaces the loop with fmodf
                 * and moves PC past it. Skipping it did not lose a
                 * measurement, it hung the game -- 4.8 billion instructions in
                 * twenty seconds, PC pinned to that loop, no frames.
                 *
                 * `observe` means the handler runs and the original
                 * instruction executes anyway, which the interception cannot
                 * express by itself: ending the block returns control to the
                 * same PC and hooks it again. DispatchHook resolves that by
                 * handing the instruction to the interpreter. */
                return SvcHook + i;
            }
        }
        return SvcNone;
    }

    /* Called once per instruction at TRANSLATION time, never at run time.
     * Returning false stops translation of the block here; the callee then
     * owns the terminal, which is why one is set below. (The declaration's
     * comment has this backwards -- the caller in translate_arm.cpp breaks on
     * !PreCodeReadHook, and its default returns true to mean "carry on".) */
    bool PreCodeReadHook(bool is_thumb, VAddr pc, Dynarmic::A32::IREmitter& ir) override {
        const std::uint32_t tag = TagFor(pc);
        if (tag == SvcNone)
            return true;

        /* PC has to be exact when the handler runs: handlers read it, and a
         * fault reported against the wrong address is worse than no address.
         * A supervisor call does not write it for us here the way a real SVC
         * does, because we are standing in front of the instruction rather
         * than translating one. */
        if (is_thumb)
            ir.UpdateUpperLocationDescriptor();
        ir.BranchWritePC(ir.Imm32(pc));
        ir.CallSupervisor(ir.Imm32(tag));
        /* Return to the dispatcher rather than linking on: the handler decides
         * where control goes (normally LR), and it does so after this block
         * was compiled, so there is nothing to link to.
         *
         * Terminating here is also what makes the interception safe against
         * dynarmic's register caching. A32CallSupervisor is not listed in
         * WritesToCoreRegister, so the get/set elimination pass does not
         * invalidate cached guest registers across it -- a handler's writes
         * would be invisible to any GetRegister later in the same block. There
         * is no "later in the same block". This is the same reason upstream's
         * own SVC translation is safe, and the reason nothing may be appended
         * after this call. */
        ir.SetTerm(Dynarmic::IR::Term::ReturnToDispatch{});
        /* One cycle for the interception, and it is not cosmetic.
         *
         * A block's cycle count is what drains the budget dynarmic was given
         * at Run() entry, and it is normally accumulated per translated
         * instruction -- which this block skipped, having broken out before
         * reading one. A zero-cycle block therefore costs nothing to execute,
         * so guest code that spins through imports (a wait loop calling one
         * per iteration, say) never exhausts the budget and Run() never
         * returns. That is a hang with a live process and no output, and it is
         * exactly what the first run on hardware did.
         *
         * It is also the accounting: guest_run charges a hook or a stub
         * g->executed++, so without this the same benchmark anchor would mean
         * more real work here than in the interpreter arm, and the comparison
         * would flatter whichever side skipped the counting. */
        ir.block.CycleCount() += 1;
        return false;
    }

    /* As dispatch_stub in interp.c, including the profiler bracket and the
     * no-op result for an import with no implementation. */
    void DispatchStub(std::uint32_t idx) {
        if (idx >= g->hle.count) {
            g->undef_pc = g->cpu.r[15];
            g->undef_insn = 0;
            status = GUEST_FAULT_UNDEF;
            Stop();
            return;
        }
        GuestHleSlot* s = &g->hle.slot[idx];
        if (g->prof)
            g->prof(idx, 1);
        if (s->fn)
            s->fn(&g->cpu, &g->mem, s->user ? s->user : g->hle.user);
        else
            g->cpu.r[0] = 0;
        if (g->prof)
            g->prof(idx, 0);
        BranchInterworking(g->cpu, g->cpu.r[GUEST_LR]);
        hle_calls++;
    }

    void DispatchHook(std::uint32_t idx) {
        if (idx >= g->hook_count) {
            status = GUEST_FAULT_UNDEF;
            Stop();
            return;
        }
        if (g->hook[idx].observe) {
            /* One instruction on the interpreter, rather than running the
             * handler here and trying to reproduce what follows.
             *
             * guest_run already implements `observe` exactly -- call the
             * handler, then step the instruction at whatever PC the handler
             * left behind, which is the whole mechanism by which
             * fast_angle_normalize skips the loop it sits on. Reimplementing
             * that here would be a second copy of a subtle rule, and the two
             * would drift. Stepping one instruction also guarantees forward
             * progress past the hook site, so control cannot come back to the
             * same PC and intercept it again.
             *
             * The interpreter counts what it retires into g->executed; the
             * interception already charged a cycle for this instruction, so
             * its count is rolled back to keep ticks the single source. */
            const std::uint64_t before = g->executed;
            const GuestStatus st = guest_run(g, 0xFFFFFFFFu, 1);
            g->executed = before;
            if (st != GUEST_OK && st != GUEST_STEP_LIMIT) {
                status = st;
                Stop();
            }
            hook_calls++;
            return;
        }
        g->hook[idx].fn(&g->cpu, &g->mem, g->hook[idx].user);
        BranchInterworking(g->cpu, g->cpu.r[GUEST_LR]);
        hook_calls++;
    }

    void CallSVC(std::uint32_t swi) override {
        /* Timing the boundary costs two CNTPCT_EL0 reads here and two more
         * inside DispatchStub. At this game's ~3900 crossings per frame that
         * is ~15 600 reads per frame, paid in every build. They are worth
         * paying only when someone is reading the numbers, so they follow the
         * same gate as the other profilers -- the Profilers setting in the
         * menu's Advanced tab -- because g->prof is set only then, and it is
         * the flag tested here. */
        const bool timed = g->prof != nullptr;
        const std::uint64_t t_enter = timed ? armGetSystemTick() : 0;
        svc_calls++;
        ring[ring_n % kRing] = Dispatch{jit->Regs()[15], jit->Regs()[14], swi};
        ring_n++;
        /* A power-of-two mask rather than a 64-bit modulo per call. */
        if (svc_calls <= 24u || (svc_calls & 0x3FFFFull) == 0ull) {
            const char *what = swi >= SvcStub ? "stub" : (swi >= SvcHook ? "hook" : "halt");
            std::printf("  [dyn  ] svc #%llu %s%u pc=%08x lr=%08x\n",
                        (unsigned long long)svc_calls, what,
                        (unsigned)(swi >= SvcStub ? swi - SvcStub
                                                  : (swi >= SvcHook ? swi - SvcHook : 0u)),
                        (unsigned)jit->Regs()[15],
                        (unsigned)jit->Regs()[14]);
        }
        if (swi == SvcHalt) {
            /* PC is already the address the caller asked to stop at, because
             * the interception wrote it before calling. */
            halted = true;
            status = GUEST_HALTED;
            Stop();
            return;
        }
        if (swi < SvcBase) {
            /* A real SVC in guest code. Nothing in this image should execute
             * one; say so rather than guessing at a handler. */
            std::printf("  [dyn  ] guest SVC #%u at %08x -- unhandled\n",
                        (unsigned)swi, (unsigned)g->cpu.r[15]);
            status = GUEST_FAULT_UNDEF;
            g->undef_pc = g->cpu.r[15];
            Stop();
            return;
        }

        if (swi >= SvcStub) {
            /* Imports are softfp. Every one of the 381 stubs takes its
             * arguments in r0-r3 and returns in r0; not one reads cpu->s[].
             * Copying the 64-word VFP file in each direction was 128 of the
             * 160 words moved per call, for nothing. The hooks below are the
             * exception -- hook_affine_compose writes s12-s15 -- so they keep
             * the full sync. */
            SyncFromJitCore();
            DispatchStub(swi - SvcStub);
            SyncToJitCore();
        } else {
            SyncFromJit();
            DispatchHook(swi - SvcHook);
            SyncToJit();
        }
        /* Only the dispatching path is charged. The early returns above are
         * halt and fault, which happen once and would not move the total. */
        if (timed)
            t_svc += armGetSystemTick() - t_enter;
    }

    /* -------------------------------------------------- everything else */

    /* Reached only for encodings dynarmic declines to translate. The
     * interpreter covers this image completely, so this is a safety net, not a
     * path. guest_run counts what it retires into g->executed; AddTicks will
     * count the same instructions again, so the interpreter's contribution is
     * rolled back here and the tick accounting stays the single source. */
    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        SyncFromJit();
        g->cpu.r[15] = pc;
        const std::uint64_t before = g->executed;
        const GuestStatus st = guest_run(g, 0xFFFFFFFFu, num_instructions);
        g->executed = before;
        if (st != GUEST_OK && st != GUEST_STEP_LIMIT) {
            status = st;
            Stop();
        }
        SyncToJit();
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        using E = Dynarmic::A32::Exception;
        switch (exception) {
        /* Hints. The guest uses them as scheduling suggestions to an OS that
         * is not here; ignoring them is the correct emulation, and the
         * interpreter ignores them too. */
        case E::SendEvent:
        case E::SendEventLocal:
        case E::WaitForInterrupt:
        case E::WaitForEvent:
        case E::Yield:
        case E::PreloadData:
        case E::PreloadDataWithIntentToWrite:
        case E::PreloadInstruction:
            return;
        /* Unpredictable is not undefined: the architecture permits any
         * behaviour, dynarmic picks one and carries on, and the interpreter
         * has been executing these encodings for 2.7 billion instructions
         * without either of us calling it an error. Killing the run here would
         * end a session that would otherwise have been fine, so count it and
         * show the first few in case they cluster somewhere interesting. */
        case E::UnpredictableInstruction:
            if (unpredictable++ < 8)
                std::printf("  [dyn  ] unpredictable encoding at %08x\n",
                            (unsigned)pc);
            return;
        default:
            break;
        }
        std::uint32_t enc = 0;
        guest_ifetch32(&g->mem, pc & ~3u, &enc);
        g->undef_pc = pc;
        g->undef_insn = enc;
        status = GUEST_FAULT_UNDEF;
        Stop();
    }

    /* Ticks are instructions: GetTicksForCode returns 1 for everything, so the
     * budget dynarmic counts down is exactly guest_run's instruction limit and
     * `used` is exactly what guest_run would have added to g->executed. */
    void AddTicks(std::uint64_t ticks) override { used += ticks; }
    std::uint64_t GetTicksRemaining() override {
        return used < budget ? budget - used : 0;
    }

    /* ---------------------------------------------------------- state */

    /* Set while cpu.s[] is known to be behind the JIT's ExtRegs, because an
     * import call skipped copying them. Only SyncToJit cares: it would
     * otherwise push that stale copy back and clobber live registers -- and
     * s16-s31 are callee-saved, so the guest can have values there across an
     * import call. */
    bool vfp_stale = false;

    void SyncToJit() {
        if (vfp_stale) {
            /* The JIT holds the truth; take it before overwriting it. */
            const std::array<std::uint32_t, 64>& cur = jit->ExtRegs();
            for (int i = 0; i < 64; i++)
                g->cpu.s[i] = cur[i];
            vfp_stale = false;
        }
        std::array<std::uint32_t, 16>& r = jit->Regs();
        for (int i = 0; i < 16; i++)
            r[i] = g->cpu.r[i];
        std::array<std::uint32_t, 64>& e = jit->ExtRegs();
        for (int i = 0; i < 64; i++)
            e[i] = g->cpu.s[i];
        jit->SetCpsr(PackCpsr(g->cpu));
        jit->SetFpscr(g->cpu.fpscr);
    }

    /* The core half of the pair, for import calls: 16 words plus flags. */
    void SyncToJitCore() {
        std::array<std::uint32_t, 16>& r = jit->Regs();
        for (int i = 0; i < 16; i++)
            r[i] = g->cpu.r[i];
        jit->SetCpsr(PackCpsr(g->cpu));
        jit->SetFpscr(g->cpu.fpscr);
    }

    void SyncFromJitCore() {
        const std::array<std::uint32_t, 16>& r = jit->Regs();
        for (int i = 0; i < 16; i++)
            g->cpu.r[i] = r[i];
        UnpackCpsr(g->cpu, jit->Cpsr());
        g->cpu.fpscr = jit->Fpscr();
        vfp_stale = true;
    }

    void SyncFromJit() {
        const std::array<std::uint32_t, 16>& r = jit->Regs();
        for (int i = 0; i < 16; i++)
            g->cpu.r[i] = r[i];
        const std::array<std::uint32_t, 64>& e = jit->ExtRegs();
        for (int i = 0; i < 64; i++)
            g->cpu.s[i] = e[i];
        UnpackCpsr(g->cpu, jit->Cpsr());
        g->cpu.fpscr = jit->Fpscr();
        vfp_stale = false;
    }

    /* One entry per 4 KB page that lies wholly inside a region, pointing at
     * the host bytes for that page. Partial pages are left null and fall back
     * to the callbacks, which is correct rather than merely safe: a page
     * half-covered by a region has no single host base.
     *
     * Read-only regions are mapped here too, so a store into the image would
     * succeed instead of faulting. That is a real difference from the region
     * path -- and it is the behaviour the port already has, because the
     * fastmem window in guest_wptr returns fast_base + addr before it ever
     * consults the writable flag. Nothing in this image writes its own code;
     * if that assumption ever needs enforcing, it needs enforcing in both
     * places at once. */
    bool BuildPageTable() {
        page_table.reset(new (std::nothrow) PageTable());
        if (!page_table)
            return false;
        page_table->fill(nullptr);
        std::size_t mapped = 0;
        for (int i = 0; i < g->mem.count; i++) {
            const GuestRegion& r = g->mem.region[i];
            const std::uint32_t first = (r.base + std::uint32_t(kPageSize - 1)) & ~std::uint32_t(kPageSize - 1);
            const std::uint32_t last = (r.base + r.size) & ~std::uint32_t(kPageSize - 1);
            for (std::uint32_t a = first; a < last; a += std::uint32_t(kPageSize)) {
                (*page_table)[a >> kPageBits] = r.host + (a - r.base);
                mapped++;
            }
        }
        std::printf("  [dyn  ] page table: %u pages (%u MB) over %d regions\n",
                    (unsigned)mapped, (unsigned)((mapped * kPageSize) >> 20), g->mem.count);
        return true;
    }
};

BozEnv g_env;
std::unique_ptr<Dynarmic::A32::Jit> g_jit;

}  // namespace

extern "C" int dyn_init(Guest* g, unsigned code_cache_mb) {
    g_env.g = g;
    if (!g_env.BuildPageTable()) {
        std::printf("  [dyn  ] page table allocation failed\n");
        return 0;
    }
    g_env.RefreshHookMap();

    {
        /* Time the W^X transition AT THE SIZE ACTUALLY USED.
         *
         * The first version of this probe used a 64 KB block, measured 4 us a
         * round trip, and I concluded the transitions were free. The real code
         * cache is 32 MB -- 512 times larger -- and this is a kernel operation
         * over a page range, so its cost scales with the region. Measuring the
         * cheap case and generalising was the whole error; the numbers below
         * are from the same allocation size the JIT really uses.
         *
         * Probed before the real Jit is constructed so the two large
         * allocations never coexist. */
        Jit probe;
        if (R_SUCCEEDED(jitCreate(&probe, (size_t)code_cache_mb * 1024u * 1024u))) {
            const u64 f = armGetSystemTickFreq();
            const u64 t0 = armGetSystemTick();
            for (int i = 0; i < 10; i++) {
                jitTransitionToWritable(&probe);
                jitTransitionToExecutable(&probe);
            }
            const u64 dt = armGetSystemTick() - t0;
            printf("  [dyn  ] jit type %d, W^X round trip at %u MB: %llu us"
                   " (a block emit pays two)\n",
                   (int)probe.type, code_cache_mb,
                   (unsigned long long)(dt * 1000000ull / f / 10ull));
            jitClose(&probe);
        }
    }

    Dynarmic::A32::UserConfig conf;
    conf.callbacks = &g_env;
    conf.page_table = g_env.page_table.get();
    conf.absolute_offset_page_table = false;
    /* Let an access that straddles a page boundary fall back to the
     * callbacks. The page table resolves one page per access, so a 4-byte load
     * two bytes before a page end would otherwise read past what that entry
     * covers -- into the next region's host bytes, or off the end of the last
     * one. The callbacks check the whole span. */
    conf.detect_misaligned_access_via_page_table = 16 | 32 | 64;
    conf.only_detect_misalignment_via_page_table_on_page_boundary = true;
    /* No exclusive monitor: the guest is single-threaded by construction. */
    conf.global_monitor = nullptr;
    conf.enable_cycle_counting = true;
    conf.code_cache_size = std::size_t(code_cache_mb) * 1024u * 1024u;

    g_jit.reset(new (std::nothrow) Dynarmic::A32::Jit(conf));
    if (!g_jit) {
        /* On Horizon this is the expected failure when the process has no way
         * to obtain executable memory, which depends on how it was launched.
         * The interpreter is still there; say what happened and let the caller
         * fall back. */
        std::printf("  [dyn  ] could not create the JIT (no executable memory?)\n");
        g_env.page_table.reset();
        return 0;
    }
    g_env.jit = g_jit.get();
    std::printf("  [dyn  ] dynarmic ready, %u MB code cache\n", code_cache_mb);
    return 1;
}

extern "C" void dyn_close(Guest* g) {
    (void)g;
    g_jit.reset();
    g_env.jit = nullptr;
    g_env.page_table.reset();
}

extern "C" void dyn_invalidate(Guest* g, uint32_t addr, uint32_t len) {
    (void)g;
    if (g_jit)
        g_jit->InvalidateCacheRange(addr, len);
}

extern "C" GuestStatus dyn_run(Guest* g, uint32_t until, uint64_t limit) {
    if (!g_jit)
        return guest_run(g, until, limit);

    g_env.g = g;
    g_env.until = until;
    g_env.budget = limit;
    g_env.used = 0;
    g_env.status = GUEST_OK;
    g_env.stop = false;
    g_env.halted = false;
    g_env.RefreshHookMap();

    /* guest_run tests `until` before executing anything, so a call that is
     * already there retires nothing and reports HALTED. The interception
     * cannot do that -- it only fires once a block is being translated -- so
     * the entry case is handled here to keep the two contracts identical. */
    if ((g->cpu.r[15] & ~1u) == (until & ~1u))
        return GUEST_HALTED;

    g_env.SyncToJit();

    /* Every block now retires at least one cycle, so Run() always returns
     * eventually and a guest loop can no longer hang inside it. What is still
     * possible is Run() returning again and again having retired nothing --
     * some halt reason raised and re-raised. Left alone that is the same
     * silent hang one level up, so it is bounded: after a few barren returns,
     * say where the guest is and give up. A diagnosis beats a lock-up even
     * when the diagnosis is only a PC. */
    std::uint64_t last_used = g_env.used;
    unsigned barren = 0;
    while (!g_env.stop && g_env.used < limit) {
        const std::uint64_t t_enter = armGetSystemTick();
        const Dynarmic::HaltReason hr = g_jit->Run();
        g_env.t_run += armGetSystemTick() - t_enter;
        g_env.runs++;
        g_env.SyncFromJit();
        if (g_env.stop)
            break;
        if (!hr)
            break;  /* budget exhausted */
        if (g_env.used == last_used) {
            if (++barren >= 16) {
                std::printf("  [dyn  ] stalled at pc=%08x %s, halt=%08x,"
                            " %llu imports %llu hooks so far\n",
                            (unsigned)g->cpu.r[15],
                            (g->cpu.cpsr & CPSR_T) ? "thumb" : "arm",
                            (unsigned)hr,
                            (unsigned long long)g_env.hle_calls,
                            (unsigned long long)g_env.hook_calls);
                g->undef_pc = g->cpu.r[15];
                g_env.status = GUEST_FAULT_UNDEF;
                break;
            }
        } else {
            barren = 0;
            last_used = g_env.used;
        }
        /* Anything else -- a cache invalidation raised from inside a callback,
         * for instance -- is cleared and execution resumes. */
        g_jit->ClearHalt(hr);
    }
    g_env.SyncFromJit();

    g->executed += g_env.used;

    if (g_env.status != GUEST_OK)
        return g_env.status;
    /* STEP_LIMIT, not OK, and NOT conditional on used >= limit.
     *
     * Dynarmic stops on a block boundary, so a 5,000,000-instruction budget
     * comes back having retired a few less. guest_run stops on an instruction
     * boundary and so returns exactly the limit. The caller's loop is
     * `while (st == GUEST_STEP_LIMIT)`, and reporting OK because the count
     * landed 3 short would end the run -- the game would exit cleanly after
     * the first chunk and look like it had simply finished. There is no other
     * way out of the loop above: a fault or a halt has already returned. */
    return GUEST_STEP_LIMIT;
}

/* Called from the control socket's thread, so it may run while the guest
 * thread is inside Run(). Reading the register file and the counters from here
 * races with that thread by construction -- but the alternative is no
 * information at all about a process that has stopped talking, and every field
 * printed is a plain word whose torn value would still be more informative
 * than silence. Call it twice: the counters moving or not moving is the
 * question it exists to answer. */
extern "C" void dyn_diag(void) {
    Guest* g = g_env.g;
    if (!g_jit || !g) {
        std::printf("  [diag ] dynarmic not active\n");
        return;
    }
    std::printf("  [diag ] svc=%llu fetch=%llu imports=%llu hooks=%llu"
                " ticks=%llu/%llu\n",
                (unsigned long long)g_env.svc_calls,
                (unsigned long long)g_env.fetches,
                (unsigned long long)g_env.hle_calls,
                (unsigned long long)g_env.hook_calls,
                (unsigned long long)g_env.used,
                (unsigned long long)g_env.budget);
    {
        const std::uint64_t f = armGetSystemTickFreq();
        const std::uint64_t run_ms = g_env.t_run * 1000ull / f;
        const std::uint64_t svc_ms = g_env.t_svc * 1000ull / f;
        std::printf("  [diag ] in Run %llu ms over %llu calls; of that,"
                    " %llu ms interceptions, %llu ms dynarmic itself\n",
                    (unsigned long long)run_ms,
                    (unsigned long long)g_env.runs,
                    (unsigned long long)svc_ms,
                    (unsigned long long)(run_ms - svc_ms));
    }
    std::printf("  [diag ] jit pc=%08x lr=%08x sp=%08x cpsr=%08x %s\n",
                (unsigned)g_jit->Regs()[15], (unsigned)g_jit->Regs()[14],
                (unsigned)g_jit->Regs()[13], (unsigned)g_jit->Cpsr(),
                (g_jit->Cpsr() & CPSR_T) ? "thumb" : "arm");
    const unsigned n = g_env.ring_n < BozEnv::kRing ? g_env.ring_n : BozEnv::kRing;
    for (unsigned i = 0; i < n; i++) {
        /* Oldest first, so a repeating cycle reads as one. */
        const unsigned idx = (g_env.ring_n - n + i) % BozEnv::kRing;
        const BozEnv::Dispatch& d = g_env.ring[idx];
        const char* what = d.tag >= SvcStub ? "stub" : (d.tag >= SvcHook ? "hook" : "halt");
        const unsigned num = d.tag >= SvcStub ? d.tag - SvcStub
                                              : (d.tag >= SvcHook ? d.tag - SvcHook : 0u);
        const char* name = "";
        if (d.tag >= SvcStub && num < g->hle.count && g->hle.slot[num].name)
            name = g->hle.slot[num].name;
        std::printf("  [diag ] %2u: %s%-3u pc=%08x lr=%08x %s\n",
                    i, what, num, (unsigned)d.pc, (unsigned)d.lr, name);
    }
}

extern "C" void dyn_report(Guest* g) {
    (void)g;
    if (!g_jit) {
        std::printf("  [dyn  ] not active\n");
        return;
    }
    std::printf("  [dyn  ] %llu HLE imports, %llu native hooks\n",
                (unsigned long long)g_env.hle_calls,
                (unsigned long long)g_env.hook_calls);
}

#else  /* !BOZ_DYNARMIC */

#include "dynarmic_glue.h"

extern "C" int dyn_init(Guest *g, unsigned code_cache_mb) {
    (void)g; (void)code_cache_mb;
    return 0;
}
extern "C" void dyn_close(Guest *g) { (void)g; }
extern "C" void dyn_invalidate(Guest *g, uint32_t a, uint32_t l) {
    (void)g; (void)a; (void)l;
}
extern "C" GuestStatus dyn_run(Guest *g, uint32_t until, uint64_t limit) {
    return guest_run(g, until, limit);
}
extern "C" void dyn_diag(void) { }
extern "C" void dyn_report(Guest *g) { (void)g; }

#endif /* BOZ_DYNARMIC */
