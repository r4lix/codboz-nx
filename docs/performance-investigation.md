# Where the frame goes when the zombies arrive

Investigation of the in-game frame-rate drop, 2026-09-19. Branch
`jit-coverage-and-chaining`, dynarmic build, handheld mode throughout.

Every claim below is tagged **[M] measured** (a number read out of
`jit/s3e_interp/nxlog.txt` or out of the image bytes), **[I] inferred** (a
conclusion drawn from measured numbers plus code reading), or **[A] assumed**
(a plausible model that has *not* been checked and must be measured before it
is believed).

---

## 0. The finding that reframes everything else

**The two consoles in the co-op session were not running at the same clock, and
neither headline number means what it looks like.** [M]

`nxlog.txt` line 1799628 and 1801649, same session, 12:11:

```
[ 0.34] @188 clocks (start): cpu 1785 MHz gpu 768 MHz emc 2333 MHz     <- Lite
[ 0.27] @157 clocks (start): cpu 1020 MHz gpu 307 MHz emc 1331 MHz     <- OLED
```

So in the Kino two-player match:

| | Lite @188 | OLED @157 |
|---|---|---|
| CPU clock | 1785 MHz (boost) | **1020 MHz (stock handheld)** |
| frame time | 26.04 ms | 36.50 ms |
| frame rate | 38.4 fps | 27.4 fps |
| guest instructions / frame | 2.494 M | 2.447 M |
| guest instructions / wall second | 95.8 M/s | 67.0 M/s |

Both consoles execute **the same work per frame to within 2 %** (2.494 M vs
2.447 M guest instructions). [M] The OLED is not doing more simulation because
it hosts the match; it is simply running at 57 % of the Lite's clock.

Two consequences, and they set the whole agenda:

1. **The honest stock-clock baseline for this scene is 27 fps, not 40.** The
   40 fps figure is a 1785 MHz number. 1785 MHz is the standard Horizon boost
   step (the one the OS grants during loading), not out-of-spec silicon abuse —
   but it is still not the handheld play clock, and you have ruled it out. [M]
2. The "60 fps in menus" figure in the brief is also a 1785 MHz measurement
   (latest session, line 23). The menu has never been measured at stock. [M]

Also: the `[dyn]` startup probe times a 32 MB W^X round trip at 5599 µs on the
Lite and 9736 µs on the OLED — a ratio of 1.739 against a clock ratio of 1.750.
[M] An unrelated kernel-side measurement scaling with the clock is independent
corroboration that the two machines differ only in clock.

---

## 1. The measured breakdown

### 1.1 Separating work from waiting

`glClear` is a bare passthrough — `t_glClear` in `jit/gl_thunks.c:162` is three
lines and calls `glClear` directly. It cannot be doing 20 % of a frame's work.
[M] It is 2 calls per frame. [M]

The clean proof that it is a *wait*, not work:

| | Lite (CPU 1785, **GPU 768**) | OLED (CPU 1020, **GPU 307**) |
|---|---|---|
| `glClear` share of frame | **19–20 %** | **3–6 %** |
| `glDrawElements` share | 7 % | 8–9 % |

The console with the **2.5× faster GPU** waits *more* in `glClear`, not less.
[M] So `glClear` is not waiting on GPU work. It is waiting on a fixed-rate
event — the vblank / back-buffer acquire after the previous `eglSwapBuffers`.
[I] The faster CPU finishes its frame sooner and spends the difference
blocked there.

This is confirmed a second way. Excluding the handler bucket entirely:

| | interpreting time / frame | guest instr / frame | effective rate | cycles / guest instr |
|---|---|---|---|---|
| Lite @1785 | 0.65 × 26.04 = 16.93 ms | 2.494 M | 147.3 M/s | **12.12** |
| OLED @1020 | 0.78 × 36.50 = 28.47 ms | 2.447 M | 85.9 M/s | **11.87** |

147.3 / 85.9 = **1.715** against a clock ratio of **1.750** — linear to within
2 %. [M] Guest execution is purely CPU-clock-bound, at about **12 host cycles
per guest instruction** on both machines. [M]

Nothing here contradicts the memory note's "the frame is 91 % CPU-bound"; it
sharpens it.

**Caveat on the fps histogram.** The Lite's fps samples pile up at exactly 40
(1130 of ~2900 samples) and the OLED's at exactly 30 (1200 samples). [M] 60,
40 and 30 are 60/1, 60/1.5 and 60/2. `eglSwapInterval` is never called anywhere
in the tree [M], so the EGL default of 1 (vsync on) applies, and the port never
configures the NWindow buffer count itself. The plateaus are consistent with a
vsync-locked swapchain quantising the present rate. [I] I could not settle the
exact swapchain behaviour from logs — see §4(b).

### 1.2 The stock-clock frame budget

OLED, 1020/307/1331, zombie-heavy Kino co-op. 36.50 ms/frame total. [M]

| term | ms/frame | % of frame | basis |
|---|---:|---:|---|
| **guest code executed by dynarmic** | **~27.3** | **75 %** | [I] interpreting bucket minus marshalling |
| HLE call marshalling (see §1.3) | ~1.2 | 3 % | [I] 3900 calls × ~320 cycles |
| `glDrawElements` (128 draws) | 2.9 | 8 % | [M] 8 % of window |
| all other HLE handlers (~3700 calls) | ~3.1 | 8 % | [I] handlers 21 % minus the named entries |
| `s3eDeviceYield` | 0.37 | 1 % | [M] |
| `eglSwapBuffers` | ~0.18 | 0.5 % | [M] |
| `glClear` — **vsync wait, not work** | ~1.1 | 3 % | [M] |
| **real CPU work per frame** | **~35.4** | | [I] |

**To reach 60 fps (16.67 ms) at stock in this scene the frame must be cut by
53 % — a 2.1× improvement.** To reach 40 fps, 29 %. To reach a solid 30 fps,
6 %. [I]

That is the honest target, and it is why the ranking below leads with
"reduce the work", not "tune the JIT".

### 1.3 The HLE boundary is crossed ~3900 times per frame

Measured from the `[dyn] svc #N` counter, which prints every 200000 calls,
against the present counter over the same span: [M]

* Lite: 37 800 000 SVC calls / 9620 presents = **3929 per frame**
* OLED: 23 400 000 SVC calls / 6159 presents = **3799 per frame**

The two agree, which is what you would expect for the same workload.

Sampling which import those calls land on (every 200000th call is logged with
its slot index; slot → name resolved through the loader's PLT/GOT/tag-4
mapping, replicating `loader/s3e_loader.c:172-250`): [M for the exact profiler
entries, ±30 % for the sampled ones, n=325]

| import | calls / frame | source |
|---|---:|---|
| `glBindBuffer` | **810** | [M] profiler, exact |
| `glDrawElements` | **128** | [M] profiler, exact |
| `glClear` | **2** | [M] profiler, exact |
| `glClientActiveTexture` | ~252 | [M] sampled |
| `glMatrixMode` | ~240 | [M] sampled |
| `glColor4x` | ~168 | [M] sampled |
| `glBindTexture` | ~156 | [M] sampled |
| `glTexCoordPointer` | ~156 | [M] sampled |
| `glColorPointer` / `glActiveTexture` | ~132 each | [M] sampled |
| `glVertexPointer` / `glLoadIdentity` / `glLoadMatrixf` | ~96 each | [M] sampled |
| `glRotatef` / `glScalef` | ~84 each | [M] sampled |
| `s3eFileRead` / `glDisable` | ~72 each | [M] sampled |
| native hooks (malloc/free/memcpy/memset/watches) | ~816 total | [M] sampled |

**6.3 `glBindBuffer` calls per draw call, and roughly 1900 pure state-setting
calls per frame ahead of 128 draws.** [M/I] This is a textbook redundant-state
profile. The thunks are all bare passthroughs — there is no state cache
anywhere in `gl_thunks.c`. [M]

### 1.4 What each HLE crossing costs, and where it is charged

`Env::CallSVC` in `jit/dynarmic_glue.cpp:423-465` does, on **every** call:

```c
const std::uint64_t t_enter = armGetSystemTick();      // 1
svc_calls++;
ring[ring_n % kRing] = Dispatch{jit->Regs()[15], jit->Regs()[14], swi};
if (svc_calls <= 24u || (svc_calls % 200000ull) == 0ull) { ... }
SyncFromJit();                                          // 16 + 64 words + CPSR + FPSCR
    ... DispatchStub -> g->prof(idx,1)                  // 2  armGetSystemTick
        s->fn(...)
        g->prof(idx,0)                                  // 3  armGetSystemTick
SyncToJit();                                            // 16 + 64 words + CPSR + FPSCR
t_svc += armGetSystemTick() - t_enter;                  // 4
```

Three separate observations, all from code reading plus the call counts above:

1. **160 register words (640 bytes) are copied per call**, of which **128 words
   are the VFP file (`s0..s63`)**. The ABI is **softfp**: `t_glRotatef`
   (`gl_thunks.c:203`) reads all four of its float arguments from
   `ga(c,m,0..3)`, i.e. core registers r0–r3, and reinterprets the bits. **No
   GL or s3e thunk reads `c->s[]` at all.** [M] The only code in the whole port
   that touches `cpu->s[]` is the *hook* `hook_affine_compose`
   (`main.c:7019-7024`), and it writes only s12–s15. So for all 381 import
   stubs the ExtRegs sync in both directions is dead weight. [I]

2. **Four `armGetSystemTick()` reads per call, unconditionally.** `g.prof =
   hle_profile` is assigned at `main.c:7449` with **no gate**; only `g.iprof`
   and `g.pcprof` are behind `profile.txt` (`main.c:7463-7481`). [M] At 3900
   calls/frame that is **15 600 CNTPCT_EL0 reads per frame** in every shipping
   build. [I]

3. **All of that is charged to "interpreting", not to "handlers".** The
   profiler bracket `g->prof(idx,1) … g->prof(idx,0)` sits *inside*
   `SyncFromJit()`/`SyncToJit()`. [M] So the reported "handlers 21 %" excludes
   the entire marshalling cost, and "interpreting 78 %" is not pure guest code.
   **Any reading of that split that treats `interpreting` as "the JIT" is
   wrong by the marshalling term.** [I]

Cost estimate, ~320 cycles/call (≈60–120 for the copies, ~160 for four CNTPCT
reads at ~40 cycles each, ~30–60 for the dispatcher round-trip after the
mandatory `ReturnToDispatch`): 3900 × 320 = 1.25 M cycles = **~1.2 ms/frame at
1020 MHz**, of which **~0.8–1.0 ms is recoverable**. [A — the per-read CNTPCT
cost is the weak link and must be measured, not assumed.]

### 1.5 Where the guest CPU time actually goes

`pcprof` is blind under dynarmic — it counts only instructions the
*interpreter* retires, and the last `[pcprof]` block in the log is from an
interpreter-era session at line ~330000. [M] So the guest hot-code profile had
to be rebuilt from the periodic `... NNNNM instructions, pc=XXXXXX` samples,
which *do* fire under dynarmic (n = 8045 across both consoles in the gameplay
window). Both consoles agree closely, which is the check that it is sampling
the same workload. [M]

By 4 KB page:

| page (RVA) | share |
|---|---:|
| `0x006000` | 9.60 % |
| `0x005000` | 6.72 % |
| `0x360000` | 5.44 % |
| `0x22f000` | 2.83 % |
| `0x378000` / `0x379000` | 2.56 % / 2.54 % |
| top 20 pages together | **52.2 %** |

**The profile is flat.** The hottest 4 KB page is 9.6 %; it takes twenty pages
to cover half the stream. [M] There is no single function to hook — and this is
the same wall the project already hit: the memory note records hooks
plateauing at ~19.2 fps under the interpreter, and under dynarmic a hook is
strictly worse (block termination + register sync + dispatcher trip in place of
natively compiled code).

The hottest cluster, pages `0x005000`–`0x006fff` at **16.3 % combined**, is
identified: disassembling `0x6500..0x6600` shows a **CPU vertex-skinning
loop** — it walks a vertex array, reads per-vertex bone indices with a `0xff`
sentinel (up to four influences), builds an influence mask, calls
`fn_0x524c` (the Q12 3×3 transform the project already knows as the top pcprof
entry), and writes 6-byte (3 × u16) transformed positions. [M]

That is exactly the code whose cost scales with the number of zombies on
screen, and it corroborates independently: the game's own shipped ICF
benchmark block at file offset `0x2364` lists
`IW_GX_METRIC_VERTS_TRANSFORMED` — this is Marmalade IwGX doing vertex
transformation on the CPU by design. [M]

Guest instruction mix (from the archived `[iprof]` block; the guest image is
unchanged so the mix still holds): `mov` (T16 46) 9 %, `ldr rd,[rn,#imm]`
(T16 68) 7 %, branch/BL 3 %, **VFP (T32 ee) 3 %**, compare 3 %, **push 3 %,
pop 3 %**, load/store (T32 f8) 2 %, **vldr/vstr (T32 ed) 2 %**. [M]

Memory access is the dominant class once `push`/`pop` (which move 4–8 words
each) are counted. [I] That matters for §2.3.

### 1.6 The dormant interpolation path — verified present in our image

`codboz_frame_interpolation.c` in the PortMaster port is **not** a frame
generator. It re-enables an animation-interpolation path that the shipped game
already contains but leaves switched off:

* The director has two callback slots, `0x3e6490` (fixed step) and `0x3e6494`
  (interpolate).
* The shipped interpolate callback at `0x0ba7da` is **`70 47` — a bare
  `bx lr`**. Interpolation is stubbed out in the retail binary. [M]
* The port points the fixed slot at a wrapper that also calls
  `capture_transforms` (`0x0e8786`), and the interpolate slot at one that calls
  `apply_factor` (`0x0e87c8`) with a Q12 alpha.

**All four signatures match our `boz.s3e.unpacked` byte for byte, and both
director slots hold exactly `link_base(0x4a000000) + callback`:** [M]

```
MATCH  DIRECTOR_FIXED_CALLBACK 0x0ba98c: 02292de9f04180460f461646dde90645
MATCH  DIRECTOR_INTERPOLATE_CB 0x0ba7da: 7047
MATCH  CAPTURE_TRANSFORMS     0x0e8786: 38b505460024ab689c4206d26b6853f8
MATCH  APPLY_FACTOR           0x0e87c8: 70b505460e460024ab689c4207d26b68
  FIXED_SLOT  0x3e6490 = 0x4a0ba98d
  INTERP_SLOT 0x3e6494 = 0x4a0ba7db
```

Disassembling the fixed callback confirms the mechanism: at `0xba9c2` it does
`vdiv.f32 s15, s14, s15` and stores the result at `[r8,#0x38]` — it is already
computing an interpolation alpha every fixed step, and currently throwing it
away. [M] It then tail-calls `0xba154`, which is inside the `0x0ba130..0x0ba1cf`
range the project's own archived pcprof ranks at 5 %. [M]

One correction to carry over: `ACTIVE_SET_POINTER_OFFSET = 0x45f2b0` lies past
the end of the *file* image (`0x41db1c`) — it is in the zero-filled BSS tail.
Any port of this must bounds-check against the loaded `image_size`, not against
the file length. [M]

**Be clear about what this buys.** It *adds* a per-object `apply_factor` pass
every frame. As a pure fps change it is slightly **negative**. Its value is
(a) large perceived-smoothness gain at 27–30 fps, and (b) it is the
prerequisite for the only large work-reduction lever available — stepping
animation at a lower rate than rendering. [I]

---

## 2. Ranked proposals

Percentages are against the ~35.4 ms of real CPU work per frame at stock.

### 2.1 — Elide redundant GL state changes in the thunks
**Gain: 4–8 % (1.5–3 ms/frame) · Risk: low–medium · Effort: medium**

~1900 pure state calls per frame ahead of 128 draws, all bare passthroughs.
[M] The context is GLES1 fixed-function on mesa, where the driver derives a
shader program from fixed-function state; every state touch dirties that
derivation and it is re-resolved at draw time. So eliding redundant state
should reduce not only the ~3.1 ms "other handlers" bucket but also a share of
`glDrawElements`' 2.9 ms. [I — the mesa-internal part is the uncertain half.]

Shadow the current value and return early when unchanged, starting with
`glBindBuffer` (810/frame), `glBindTexture`, `glMatrixMode`,
`glClientActiveTexture`, `glActiveTexture`, `glEnable`/`glDisable`,
`glShadeModel`, `glColor4x`.

Care required: `glBindBuffer` state is per-target and changes the meaning of
subsequent `gl*Pointer` calls; `glActiveTexture`/`glClientActiveTexture` select
which unit later calls affect; the shadow must be invalidated on context change
and around the port's own overlay drawing in `gl_egl.c` (`fps_overlay`,
`touch_overlay`, `menu_render`), which issues GL calls behind the guest's back.

**Measure:** per-slot call counts before and after (§4a) — the elided imports'
counts reaching the driver should fall sharply — and `[bench]` **presents** at
a fixed instruction count, which is the work-per-frame axis.

### 2.2 — Stop paying per-HLE-call instrumentation and the VFP sync
**Gain: 2.3–2.8 % (0.8–1.0 ms/frame) · Risk: very low · Effort: low**

Three independent, surgical changes in `jit/dynarmic_glue.cpp:423-465` and
`main.c:7449`:

1. **Split the sync by call kind.** For `swi >= SvcStub` (all 381 imports) sync
   only r0–r15 + CPSR; keep the full ExtRegs sync for `SvcHook`, because
   `hook_affine_compose` writes s12–s15. Better still, sync only s12–s15 for
   that one hook. This removes 128 of 160 words on ~3100 of 3900 calls/frame.
   Justified by softfp — see §1.4(1). [I]
2. **Gate `g.prof` on `profile.txt`** the way `g.iprof`/`g.pcprof` already are,
   and make `t_enter`/`t_svc` conditional on the same flag. That is 4
   CNTPCT_EL0 reads per call, 15 600 per frame, removed from play builds. [M]
3. Drop the `svc_calls % 200000ull` 64-bit modulo and the ring write from the
   ungated path (or mask with a power of two).

The largest single piece here is the timer reads, and it is the cheapest to
remove.

**Measure:** `[bench]` **M/s** must move; presents should not. Two runs per
arm. This is exactly the kind of change the deterministic bench was built for.

### 2.3 — Dynarmic memory-path configuration
**Gain: 2–6 % · Risk: medium · Effort: low–medium**

Memory access dominates the instruction mix [M], and the project's own measured
rule is that only **dependency-chain** shortening pays. The current config
(`dynarmic_glue.cpp:631-645`) makes every guest access:
`idx = addr>>12` → `base = pt[idx]` (dependent load) → `ptr = base + (addr &
0xFFF)` → the data load.

* **`absolute_offset_page_table = true`** folds the low-bits add into the
  addressing mode (`ldr val,[base, addr]`), removing one ALU op *from the
  dependency chain*. Requires `BuildPageTable` to store `host_page_base −
  guest_page_base`. Small but free. [I]
* **Re-examine `detect_misaligned_access_via_page_table = 16|32|64`.** It emits
  a check on every 16/32/64-bit access. The guest space is one contiguous
  `svcMapMemory` window, so adjacent guest pages *are* adjacent in host memory
  *within a region*; the hazard the flag guards only exists at region
  boundaries. Padding each region by 8 bytes of guard would let the flag be
  dropped entirely. **Do not do this blind** — validate against
  `jit/hostdiff` first.

**Measure:** `[bench]` **M/s**, and a full `hostdiff` differential run before
and after. Non-negotiable: this touches correctness.

### 2.4 — Unsafe FP optimizations, behind a card flag
**Gain: 1–4 % · Risk: medium (determinism) · Effort: very low**

`conf.optimizations` is left at `all_safe_optimizations` and
`unsafe_optimizations` is false. [M] VFP is ~5 % of instructions, and the
project has already measured that multiply/FP-heavy code costs *above* average
per instruction (the Q12 hook returned +7.4 % for a 6 % instruction share).

`Unsafe_InaccurateNaN` and `Unsafe_IgnoreStandardFPCRValue` are the two worth
trying. Skip `Unsafe_UnfuseFMA` — the A57 has FMA, so unfusing is a loss.
`Unsafe_IgnoreGlobalMonitor` buys nothing; the monitor is already `nullptr`.

**This changes FP results and therefore breaks the byte-identical-frames
determinism the differential harness relies on.** It must be a card flag,
default off, and forced off whenever `hostdiff`/verification is in play — the
same discipline `chain.txt` already follows under `jitverify.txt`.

**Measure:** `[bench]` M/s with the flag on vs off, same build.

### 2.5 — Retire the diagnostic observe hooks under dynarmic
**Gain: 0.3–0.8 % · Risk: very low · Effort: very low**

`hooks[3] watch_image_handler`, `hooks[7] watch_truncated_ptr` and
`hooks[8] watch_struct_copy` are pure diagnostics but are installed
unconditionally (`main.c:7607-7621`). [M] `hook7` alone samples at ~84
calls/frame. [M] An `observe` hook is the most expensive kind: block
termination, full register sync both ways, *and* a delegated single-instruction
interpreter step.

Gate all three behind `watch.txt`. **`hooks[4] fast_angle_normalize` must
stay** — the memory note records that skipping it hangs the game hard; it is
not a diagnostic despite being marked `observe`.

### 2.6 — Re-enable the game's own animation interpolation
**Gain: negative on fps; large on perceived smoothness · Risk: medium · Effort: medium**

Signature-verified applicable to our image (§1.6). [M] Port
`codboz_install_frame_interpolation` with the signature checks intact — they
are already written the right way (byte-compare before patching, never a bare
offset), which satisfies the project rule. Bounds-check `0x45f2b0` against
`image_size`, not file length.

Do this **for the 27–30 fps experience**, not for the frame rate, and say so in
the commit. Its strategic value is that it unlocks §2.7.

### 2.7 — Decouple the animation/skinning rate from the render rate
**Gain: potentially 8–15 % · Risk: high · Effort: high · Speculative**

The 16.3 % of guest samples in the skinning cluster (`0x005000`–`0x006fff`)
[M] is the largest single identifiable block of zombie-scaling work. The
director already runs on a fixed step and already computes an alpha it
discards [M]. If the fixed step can be halved in rate with §2.6 filling the
gap visually, roughly half that cluster's cost disappears.

**This is the only lever found that attacks the 2.1× gap rather than nibbling
at it.** It is also the least understood: I have not established what sets the
fixed-step rate, nor whether skinning is driven from the director at all.
Treat as a research task, not a plan. [A]

### 2.8 — Core affinity and priority for the guest thread
**Gain: 0–3 %, genuinely unknown · Risk: very low · Effort: very low**

`[sched] core 0, 0 migrations seen, prio 44` in every window. [M] Nothing sets
an affinity anywhere (`main.c:8041-8052` reports it and says so). The mixer is
on core 2 at prio 0x2E, the control thread on core 1 at prio 0x3B. [M]

"0 migrations" means the thread is not being bounced, so the upside is limited
to escaping whatever system work shares core 0. Worth one cheap A/B behind a
card flag (`svcSetThreadCoreMask` to core 1 or 2). Low expectations.

### 2.9 — Present pacing
**Gain: ~0 at stock · Risk: low · Effort: low · Do for instrumentation only**

`eglSwapInterval` is never called; EGL's default of 1 applies. [M] At stock the
`glClear` wait is only ~1.1 ms/frame, so there is almost nothing to reclaim —
and disabling vsync would tear. The reason to touch this path is to
**instrument** it (§4b), not to change it.

---

## 3. Dead ends — do not retry

* **True fastmem (`UserConfig::fastmem_pointer`).** It requires a **4 GB** host
  reservation and relies on **host page faults** for fallback. [M, from
  `dynarmic/src/dynarmic/interface/A32/config.h:180-188`] Horizon has no
  signal-based fault handler for the JIT, and `virtmem` probing already found
  4 GB / 2 GB / 1 GB unavailable and only 512 MB obtainable. Both halves of the
  requirement are unmeetable. The page table is the ceiling on this platform.
* **Threading the guest.** 391 imports and not one thread, mutex or atomic
  among them — the guest is single-threaded by construction, so nothing can
  split the instruction stream that is 75 % of the frame. The archived
  "threaded dispatch, 1.46×" note was an *interpreter* idea and is obsolete
  under dynarmic.
* **Reducing the real-time clock queries.** 19–27 per frame [M], i.e. ~0.6 % of
  3900 HLE calls and well under 0.02 ms/frame. The 15.4/frame figure in the
  brief is a **game-speed correctness** diagnostic (the fixed-clock bug), not a
  performance one. Leave it alone.
* **Texture upload / format conversion.** `t_glTexImage2D`,
  `t_glTexSubImage2D`, `t_glCompressedTexImage2D` are direct passthroughs with
  a pointer translation and no conversion [M], and texture calls do not appear
  in the per-frame sample at all (< 36/frame). Not a steady-state cost; it is a
  load-time hitching question.
* **More native hooks on hot guest functions.** The gameplay profile is flat —
  top 4 KB page 9.6 %, twenty pages for half the stream [M]. Combined with the
  project's own measured plateau at ~19.2 fps under the interpreter and the
  fact that a hook under dynarmic swaps compiled code for compiled code plus a
  register sync, there is nothing here.
* **GPU clock / GPU work.** The Lite ran its GPU at 768 MHz (2.5× stock) and
  still waited *more* in `glClear` than the stock-GPU OLED [M]. The GPU is not
  the constraint.
* **`pcprof` under dynarmic.** It counts only interpreter-retired instructions,
  so it is structurally blind here [M]. Do not read it, and do not "fix" it by
  turning the JIT off to profile — the profile you get then is the
  interpreter's, not the shipped configuration's.
* **Blaming the OLED.** It is not slower; it is at 1020 MHz while the Lite is
  at 1785 MHz, and once clock is divided out the two agree to within 2 % on
  both work-per-frame and cycles-per-instruction. [M]

---

## 4. Close these measurement gaps first

The profiler currently cannot answer the questions the ranking depends on.
All four are small changes.

**(a) Print the top slots by CALL COUNT, not only by time.** `g_prof_slot_calls[512]`
is *already populated for every slot* (`main.c:3574-3575`); `frame_profile_report`
just never prints it — it ranks the top 5 by ticks only (`main.c:3625-3647`).
Adding a second loop that ranks the top 15 by `g_prof_slot_calls` is a
~10-line change and immediately exposes the whole GL state storm that §1.3 had
to reconstruct by sampling. **Do this before anything else.**

**(b) Make the handler/marshalling/guest split honest.** Move the profiler
bracket to wrap the whole of `CallSVC` (or add a third bucket) so
`SyncFromJit`/`SyncToJit` stop being charged to "interpreting" (§1.4(3)). While
there, time the block around the first `glClear` after a swap separately from
the rest, which settles the vsync question in §1.1 directly instead of by
inference.

**(c) Get a guest hot-code profile that works under dynarmic.** The `pc=`
sampler used here is crude (n=8045 for a whole session). Dynarmic's
`PreCodeTranslationHook` or a periodic sampler thread would give a real one.

**(d) Reach a zombie-heavy scene deterministically.** `bench.txt` **suppresses
real input** (memory note, and `main.c:7635`-ish rationale), so it can only
reach "settled gameplay" at 5000M via the synthetic tap — *not* the pathological
many-zombies case this whole investigation is about. The control socket already
accepts `SND KEYS`; a scripted input sequence over it would give a repeatable
combat scene. Until then, CPU-throughput deltas (§2.2–2.5) are safe to judge on
the existing bench because they are scene-independent to first order, but the
GL work (§2.1) is **not** and needs the scripted scene.

### The standard paired-bench procedure

Card contents: `bench.txt` (optionally holding a smaller millions count while
iterating), `dynarmic.txt`, `fastmem.txt`. **No `profile.txt`** — it taxes every
guest instruction. **No `fixedclock.txt`** — it is the "zombies feel sped up"
bug. No `jit.txt` / `chain.txt`.

1. Run each arm **twice**; single runs have misled this project repeatedly.
2. Check `clocks (start)` **and** `clocks (end)` are identical across all four
   runs. §0 is what happens when nobody checks.
3. Read `[bench] NNNNM instructions in NNNN ms = NN.N M/s, NNN presents`.
4. **`M/s` is the axis for CPU-throughput changes** (§2.2, §2.3, §2.4, §2.5,
   §2.8). **`presents` at the fixed instruction count is the axis for
   work-per-frame changes** (§2.1, §2.6, §2.7). A change that moves fps but
   neither of these has changed the workload, not the port.
5. Any change under §2.3 additionally requires a clean `jit/hostdiff` run.

---

## 5. Do this first

1. **Print per-slot call counts (§4a) and fix the handler/marshalling split
   (§4b).** Ten lines apiece, no risk, and every proposal below is currently
   being ranked on inference that these would turn into measurement.
2. **Strip the per-HLE-call instrumentation and the VFP sync (§2.2).** Lowest
   risk on the list, the mechanism is understood rather than guessed, it is
   ~0.8–1.0 ms/frame, and it is measurable on the bench you already have.
3. **Elide redundant GL state, starting with `glBindBuffer` (§2.1).** 810 calls
   per frame for 128 draws is the single most conspicuous number in the whole
   investigation, and it is the largest tractable non-guest term.

And one thing to settle before planning further: **re-measure the menu and one
gameplay scene at 1020/307/1331 on a single console.** Every figure in the
brief is a boost-clock figure taken on a machine that was not at stock, and the
target ("60 fps in menus, drops in gameplay") has never actually been observed
at the clock you intend to ship at.

> **Requested hardware run** (the console was in use, so nothing was run for
> this report): with the card configured as in §4, one `[bench]` pair on
> **one** console at stock 1020/307/1331 — current `HEAD` versus the same build
> with §2.2 applied. Report both `[bench]` lines and the `clocks (start)` /
> `clocks (end)` pairs. That single experiment validates or kills the cheapest
> proposal and, more importantly, establishes the first honest stock-clock
> baseline this project has.
