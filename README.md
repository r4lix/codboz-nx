# codboz-nx

Running *Call of Duty: Black Ops Zombies* (Marmalade `.s3e`, ARMv7 + GLES1,
Android 2011) on the Nintendo Switch, by interpreting the ARM image and
bridging Marmalade's s3e API to libnx and mesa.

The game renders its real menus through GLES1 on the Switch GPU and takes
touchscreen input.

**No game data is in this repository.** The `.apk`, the `.obb`, and the `.s3e`
image are Activision's and are not ours to redistribute. Supply them from your
own copy — see *Runtime data* below.

## Layout

| path | what |
|---|---|
| `jit/interp.c` | ARMv7-A / Thumb-2 interpreter, the reference execution path |
| `jit/jit.c` | incremental ARM → AArch64 basic-block JIT (Switch only) |
| `jit/guest.{c,h}` | CPU state, sparse guest memory, HLE dispatch |
| `jit/gl_thunks.c` | 247 generated GL/EGL entry points |
| `jit/gl_egl.c` | hand-written EGL layer + GL overrides that the generator cannot express |
| `jit/s3e_interp/` | the Switch NRO: s3e HLE, display, input |
| `jit/hostdiff/` | the differential harness — see below |
| `loader/run_boz.py` | Unicorn reference implementation, the correctness oracle |
| `loader/mkglthunks.py` | generates `gl_thunks.c` from the GLES/EGL headers |
| `loader/s3e_loader.{c,h}` | `.s3e` container parser and relocator |

## The differential harness

Correctness rests on differential testing against Unicorn, not on the game
looking right. Both sides emit one `{pc, state_hash}` record per instruction and
`jit/hostdiff/compare.py` finds the first genuine divergence.

Two things make it usable at depth:

- **Landmark-anchored windows.** Instruction index is not a valid anchor —
  Unicorn does not fire `UC_HOOK_CODE` for an IT-block instruction whose
  condition fails, while the interpreter counts every step, so the two indices
  drift without bound. Windows anchor on something both sides count natively:
  `BOZ_TRACE_AT=<present#>` or `BOZ_TRACE_PC=<addr>` + `BOZ_TRACE_NTH=<n>`.
- **Hookless fast-forward.** Unicorn runs ~61M instr/s with no per-instruction
  hook and ~6k/s with one, so it fast-forwards to the landmark, then traces.

This is what found the `UMLAL`/`SMLAL` bug: the Thumb-2 decoder dropped the
accumulate, silently corrupting an RNG 2.5 billion instructions into a run.

`BOZ_WATCH=<addr>` logs every change to one guest word on both sides
(`watchdiff.py` diffs them) — the register hash cannot see a store that goes to
the wrong address until something loads it back.

**The harnesses must answer the SDK identically.** `run_boz.py` and
`jit/hostdiff/host_main.c` are kept in step by hand; when they drift, the
differential reports false divergences. Run both with `BOZ_TAP=0` — their
synthetic-input models differ.

## Building

Switch NRO (devkitPro, `libnx`, `mesa`/`nouveau`):

```
cd jit/s3e_interp && make
```

Host differential harness (MSVC):

```
jit\hostdiff\build.bat
```

## Runtime data

Extract from your own copy of the game and place on the SD card:

```
sdmc:/switch/boz/boz.s3e.unpacked      the LZMA-decompressed .s3e image
sdmc:/switch/boz/boz_files.idx         built by loader/mkfileidx.py
sdmc:/switch/boz/blackops_gles1.obb    from the APK's assets
sdmc:/switch/s3e_interp.nro
```

Optional: `sdmc:/switch/boz/nxlink_host.txt` containing your PC's IPv4 address.
The NRO reads it and streams its log there — run `jit/s3e_interp/nxlog.py` to
listen on port 28771. This works when launching from hbmenu, unlike
`nxlinkStdio()`, which only connects if netloader started the process.

## Status

Working: CPU verified against Unicorn to 2.765B instructions; EGL/GLES1 context
on hardware; menus rendering with correct fonts and layout; touchscreen
(handheld) and stick+A (docked) input; ~7 fps interpreted.

Open: real textures never reach GL (only the engine's 2×2 default is uploaded —
a resource-loading question, not a format one; the driver does expose ETC1);
NULL-object faults from subsystems that return 0 where the game expects a
constructed object; no audio output.
