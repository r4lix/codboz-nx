# s3e loader smoke test

Runs `s3e_loader.c` on real Switch hardware. It does **not** run the game —
that needs an ARM32 JIT which does not exist yet. It exercises every line of
Switch-native loader code we have.

## What it proves

- **Unaligned 32-bit reads on real AArch64.** The image sits at an odd file
  offset (`K = 0x39483`), so every word access in the loader is unaligned.
  This is the thing most likely to behave differently on hardware than under
  x86 Python, and it is why the entry-word check is in the output.
- A ~4.9 MB allocation under Horizon's allocator.
- The typed block chain, 52,075 relocations, the PLT scan, tag-4 symbol naming,
  and the bind + unload passes.

## Install

Copy two files to the SD card:

```
/switch/s3e_smoke/s3e_smoke.nro     <- loader/s3e_smoke/s3e_smoke.nro
/switch/boz/boz.s3e.unpacked        <- codboz_nx/boz.s3e.unpacked
```

The image is also found at `/boz.s3e.unpacked` or `/switch/boz.s3e.unpacked`;
the app prints each path it tried. `boz.s3e.unpacked` is `assets/boz.s3e` from
the APK with the LZMA-alone wrapper removed — regenerate it with the snippet in
`../REVERSING.md` if it goes missing. It is not shipped in the repo.

Launch from the homebrew menu.

## Expected output

Every line must match the PC reference run (`../s3e_loader_check.py`):

```
K (image_off) : 0x039483
link base     : 0x4a000000
image size    : 0x4a7dc8
entry         : 0x4a000000 (ARM)
relocations   : 52075
import names  : 391
GOT slots     : 381
named slots   : 378
entry words   : e59f008c e92d4010   [OK]
first GOT slot: 0x411020 = eglQueryString
bind + unload OK
```

Any mismatch is a genuine hardware-vs-PC difference and worth chasing — the
numbers are deterministic, so there is no tolerance here.

## Build

```
DEVKITPRO=/opt/devkitpro make
```

The Makefile is the stock devkitPro application template with `SOURCES` set to
`source ..` so it picks up `../s3e_loader.c`, and `INCLUDES` set to `..`.
