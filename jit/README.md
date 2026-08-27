# ARM32 CPU layer

The game is ARMv7-A (mostly Thumb-2, ARM entry stub). The Switch is AArch64-only
and Horizon fixes execution state per process, so guest code cannot run natively
inside a 64-bit NRO. This directory is the CPU that executes it.

This is the large piece of the port. Everything else — loader, archives, file
layer, allocator, framebuffer — is done or small. See `../loader/REVERSING.md`.

## Decision: interpreter first, JIT second

Do **not** start with Dynarmic. Start with a plain Thumb-2 interpreter, because:

- **Correctness is the risk, not speed.** A subtly wrong carry flag surfaces as
  a crash 400 million instructions later. Chasing that inside a recompiler is
  brutal; inside an interpreter it is a breakpoint.
- **We have a reference implementation.** `../loader/run_boz.py` runs the real
  image under Unicorn and reaches the main loop. Differential testing against it
  is the single biggest de-risker available and only works if our CPU can be
  stepped and inspected.
- **Speed is not yet needed.** A 2011 phone game on a Tegra X1 has enormous
  headroom. Get it correct and rendering, then make it fast.

Swap in a JIT at phase 4, once a correct oracle exists to test against.

## Scope, measured not guessed

Static histogram of the 3.4 MB code region (`capstone`, skipdata):

| pass | decoded | skipped | reading |
|---|---|---|---|
| Thumb | 1,429,526 | 37,468 (2.6%) | correct |
| ARM | 716,133 | 152,219 (21%) | misparsed data |

- **86 mnemonics cover 99%** of Thumb-2 code.
- The top 18 cover **87.6%**, and they are all ordinary:
  `mov b add ldr str bl blx cmp asr pop lsl sub push lsr strb cbz strh and`.
- **VFP/NEON is 3.0%** — small, but required; the ABI is softfp (args in core
  registers) while arithmetic uses VFP, so it cannot be skipped.
- Plus the ARM (A32) entry stub and interworking: the binary is mixed, so
  `bx`/`blx` must switch state on bit 0.

So the target is roughly "90 Thumb-2 mnemonics + interworking + single-precision
VFP", not "all of ARMv7".

## Architecture

```
   guest image (0x4a000000, from s3e_loader.c)
        |
   [ CPU: interpreter -> later JIT ]  <-- this directory
        |  traps when PC enters the stub page
   [ HLE: 381 GOT slots -> C handlers ]  <-- ../gen/ has the 391-name table
        |
   loader / DTRZ files / allocator / framebuffer
```

**HLE boundary is already solved.** `s3e_load()` gives 381 GOT slots and their
import names. Point every slot at `STUB_BASE + 4*i`; when the CPU is about to
execute in that range, dispatch to the C handler for slot *i* and return to LR.
Identical to what the Python harness does, so handlers port over one for one.

**Memory.** Guest is 32-bit but sparse: image ~4.9 MB at 0x4a000000, stack, heap
(~64 MB working set), framebuffer, stub page. Do not reserve 4 GB. Use a small
region table and translate `guest -> host` per access; revisit with fastmem only
if profiling demands it.

**Callbacks re-enter the CPU.** The game registers 12 callbacks and Marmalade
pumps them at `s3eDeviceYield`. Save all registers + CPSR, set r0=systemData,
r1=userData, LR=landing pad, run until PC hits the pad, restore. Proven in the
Python harness; design it in from the start rather than bolting it on.

**W^X (phase 4 only).** Horizon forbids RWX. When the JIT arrives, use
`svcCreateCodeMemory` with an RX mapping at the execute address and a separate RW
alias for the emitter — the pattern already working in `DrasticDS_nx`
(`so_util.c` `SoJitRange`, `drastic_jit.c`).

## Phases

| # | Work | Size |
|---|---|---|
| 0 | Scaffold: guest context, memory regions, HLE trap table wired to the 391 names | small |
| 1 | Thumb-2 interpreter, ~90 mnemonics + interworking, differentially tested vs Unicorn | **large** |
| 2 | Single-precision VFP (3% of code, 100% required) | medium |
| 3 | Run on Switch: correctness only, expect it to be slow | small |
| 4 | Replace the interpreter with a JIT (Dynarmic A32->A64, or custom) | large |

Phase 1 is the real work. Phases 0 and 3 are days.

## Differential testing

The point of keeping the Python harness alive. Run the same image in both, stop
both at each basic-block boundary, compare r0-r15 + CPSR (+ VFP in phase 2).
First divergence is the bug, with the exact instruction in hand. Without this,
phase 1 is guesswork; with it, it is mechanical.

## Risks

- **Flag semantics.** Carry out of shifts, `adc`/`sbc` borrow, and the flag-
  setting vs non-flag-setting Thumb encodings are where interpreters get subtly
  wrong. Differential testing is the mitigation.
- **Interworking.** Mixed ARM/Thumb means every indirect branch must honour bit
  0. Getting this wrong fails immediately and loudly, which is the good case.
- **Unaligned access.** Already proven fine on hardware by the smoke test, but
  guest loads must not be turned into host pointer casts.
- **Self-modifying code.** Not observed, but a JIT (phase 4) needs an
  invalidation story before it can be trusted.
