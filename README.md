# codboz-nx

<img width="2033" height="816" alt="2048-1536-max" src="https://github.com/user-attachments/assets/c569b9d5-83f9-47e4-a3ba-48d2f9d8eabe" />

*Call of Duty: Black Ops Zombies* (Activision, 2011 — a Marmalade `.s3e`
game for ARMv7 Android) running natively on the Nintendo Switch as homebrew.

The game's own ARM code runs on [Dynarmic](https://github.com/azahar-emu/dynarmic)
(ARMv7 → AArch64 recompilation), and everything it asks of its SDK —
graphics, sound, files, input, network — is answered by this port through
libnx and Mesa. Nothing is emulated at the device level: the game renders
through the Switch GPU with OpenGL ES 1.1.

**No game data is in this repository or in the release.** The APK, OBB and
`.s3e` image are Activision's; supply them from your own copy (see
[Installing](#installing)).


> [!IMPORTANT]
> codboz-nx is a personal project intended as a temporary solution until a more official or robust alternative becomes available.
> This project was created **entirely with the help of AI coding tools**.
> I am **not a developer**, so the code may contain bugs, security vulnerabilities, mistakes, or unfinished features. romm-nx is a personal project that I decided to share with others who may find it useful.
> Use it at your own risk. I cannot guarantee support, stability, compatibility, security, or regular updates.

## Status — v1.0.0

Playable:

- Single player with **saves** and profile progress
- **Play Online** through the community server used by the PS Vita and
  PortMaster ports, and **local Wi-Fi co-op** between Switches
- **Controller** (console layout, the game's own gamepad mode) and **touch**
- **Sound effects and music**
- 1280×720 in handheld and docked, **60 fps** in menus and light scenes
- An in-game **settings menu** (hold **−** for two seconds)

Known limits:

- Busy rounds need more CPU than a 60 fps frame allows at stock clocks, so
  they drop toward 30–45 fps; see [Frame rate](#frame-rate).
- A short hitch the first time new code or assets load (a round change, a
  new area). Later passes over the same code are smooth.
- There is no jump button: nothing in the game's input handling jumps.

## Installing

You need a Switch running custom firmware with the homebrew menu.

1. Copy `codboz.nro` to `sdmc:/switch/boz/codboz.nro`.
2. From your own copy of the game, put these in `sdmc:/switch/boz/`:

   | file | where it comes from |
   |---|---|
   | `boz.s3e.unpacked` | `boz.s3e` from the APK, LZMA-decompressed (4 550 559 bytes; the port checks it) |
   | `blackops_etc.dz` | the ETC texture pack from the game data |
   | `blackops_loader.dz` | the loader pack from the game data |
   | `boz_files.idx` | built by `loader/mkfileidx.py` from those packs |
   | `blackops-music/`, `deadops-music/` | the MP3 folders from the APK's assets |

3. Launch **CoD Black Ops Zombies** from the homebrew menu.

Saves, settings and the online account key are kept under `sdmc:/switch/boz/`
(`save/`, `config.txt`, `device-id.bin`).

## Controls

| button | action |
|---|---|
| Left stick | move — click to sprint (or hold, see settings) |
| Right stick | look / aim |
| ZR | shoot |
| ZL | aim down sights (hold, or toggle in settings) |
| Y | tap: reload · hold: buy, repair, open |
| X | change weapon |
| A / D-pad down | crouch / prone |
| R | grenade |
| L / D-pad up | tactical grenade |
| Right stick click | melee |
| D-pad left | alternate fire |
| D-pad right | reload |
| + | pause |
| − (hold 2 s) | settings menu |
| − and + (hold 1 s) | quit |

The touchscreen works throughout, and in the menus it is the easiest way to
navigate.

## The settings menu

Hold **−** for two seconds. **L/R** change tab, **A** selects, **B** closes.
Settings are saved to `sdmc:/switch/boz/config.txt` when the menu closes.

- **Controls** — console or touch layout, hold/toggle for aim and sprint,
  the Y-hold delay, and aim speed, **separately for horizontal and vertical**
  (the game turns more slowly up and down than sideways).
- **Online** — the Play Online server and your player name (applied at the
  next launch).
- **Display** — FPS counter, **frame rate cap (60 or a steady 30)**, hide the
  on-screen sticks, music.
- **Advanced** — development switches that apply at the next launch:
  translation cache size, profilers, fixed clock, touch markers, benchmark
  mode. If a launch dies before it finishes starting, the next one ignores
  this tab once (safe mode) and says so.

## Frame rate

The game holds 60 fps in menus and quiet moments. With a
crowd of zombies each frame needs 15–25 ms of CPU even at 1785 MHz — around
three quarters of it the game's own code, mostly skinning every zombie on the
CPU — which is more than the 16.7 ms a 60 fps frame has. The frame rate then
alternates between 60 and 30 and motion judders.

Two ways to handle it:

- **Display → Frame rate → 30 fps (steady).** Every frame is exactly 33.3 ms;
  in testing 296–299 of every 300 frames landed on it. Smoother than an
  uneven 40–50.
- **Raise the CPU clock** with an overclock tool such as sys-clk. The GPU is
  not the limit; the CPU is.

## Online

Play Online connects to `boz-online.xubi.org`, the rebuilt Demonware
service shared with the PS Vita and PortMaster ports
([cod-boz-online](https://github.com/Producdevity/cod-boz-online)); set a
different server in the Online tab. Local Wi-Fi co-op finds other Switches
on the same network. Most home networks do not pass multicast between
Switches, so the port also asks every address on the subnet directly.

## Building

Needs devkitPro (devkitA64, libnx, `switch-mesa`, `switch-libdrm_nouveau`)
and a Dynarmic checkout built for Horizon **beside** this repository:

```
<parent>/codboz_nx      this repository
<parent>/dynarmic       azahar-emu/dynarmic, patched and built in build-nx/
<parent>/boost_hdr      Boost headers (only header-only icl and variant are used)
```

The Horizon patches (W^X dual mapping through libnx `jit`, and build fixes)
are idempotent scripts in `jit/dynarmic_patches/`; the CMake invocation and
its traps are in the comment block of `jit/s3e_interp/Makefile`. Then:

```
cd jit/s3e_interp && make
```

produces `codboz.nro`. `make DYNARMIC=0` builds without Dynarmic; the game
then runs on the built-in interpreter at single-digit frame rates.

## Development

- **Log:** set `nxlink_host=<your PC's IPv4>` in `config.txt` and run
  `jit/s3e_interp/nxlog.py`. It works when launched from hbmenu, and each line
  carries a timestamp and the console's address.
- **Frame pacing:** every 300 frames the log prints `[pace ]` (frame-time
  percentiles, the vsync wait, frames per vsync interval) and `[hitch]` (the
  three slowest frames with what they did). With **Profilers** on, each slow
  frame also names the SDK calls that took its time, and `[prof ]`/`[calls]`
  rank every import.
- **Control socket:** the game listens on TCP 28772 while it runs.
  `jit/boz_ctl.py <ip> ls|get|put|putfile|del` reads and writes files under
  `sdmc:/switch/boz` (a new build can be deployed that way). `SND ...` commands
  drive a running game, for example `SND MENU 1` to open the settings menu, and
  `DIAG` dumps Dynarmic's state.
- `docs/performance-investigation.md` is the measured survey of where a
  frame's time goes, with the proposals not yet done.

## Layout

| path | what |
|---|---|
| `jit/dynarmic_glue.cpp` | runs the guest on Dynarmic: page table, HLE stub page, native hooks |
| `jit/dynarmic_patches/` | the Horizon patches applied to the Dynarmic checkout |
| `jit/interp.c` | ARMv7-A / Thumb-2 interpreter: the Unicorn-verified reference, the fallback when Dynarmic cannot start, and what runs the instruction at observe hooks |
| `jit/guest.{c,h}` | CPU state, sparse guest memory, HLE dispatch |
| `jit/gl_thunks.c` | 247 generated GL/EGL entry points |
| `jit/gl_egl.c` | hand-written EGL layer and the GL overrides the generator cannot express |
| `jit/s3e_files.c` | the file layer: archive index, loose files, the in-memory save cache |
| `jit/s3e_interp/source/` | the NRO: s3e HLE (`main.c`), audio, music, network, zero-conf, settings menu |
| `jit/hostdiff/` | the PC differential harness |
| `loader/` | `.s3e` loader, Unicorn reference (`run_boz.py`), `mkfileidx.py`, `mkglthunks.py`, format notes |

## Correctness: the differential harness

The interpreter was made correct by differential testing against Unicorn, not
by the game looking right. `loader/run_boz.py` (Unicorn) and
`jit/hostdiff/host_main.c` (the interpreter, built with MSVC by
`jit\hostdiff\build.bat`) each emit one `{pc, state_hash}` record per
instruction, and `jit/hostdiff/compare.py` finds the first genuine divergence.
They agree past 2.7 billion instructions. Dynarmic reuses the interpreter's
HLE, hooks and memory map unchanged, so what was verified there carries over.

Two things make it usable at depth:

- **Landmark-anchored windows.** Unicorn does not fire `UC_HOOK_CODE` for an
  IT-block instruction whose condition fails, while the interpreter counts
  every step, so instruction indices drift. Windows anchor on something both
  sides count: `BOZ_TRACE_AT=<present#>` or `BOZ_TRACE_PC=<addr>` +
  `BOZ_TRACE_NTH=<n>`.
- **Hookless fast-forward.** Unicorn runs ~61M instr/s without a
  per-instruction hook and ~6k/s with one, so it fast-forwards to the
  landmark, then traces.

`BOZ_WATCH=<addr>` logs every change to one guest word on both sides
(`watchdiff.py` diffs them). The two harnesses must answer the SDK
identically; run both with `BOZ_TAP=0`.
