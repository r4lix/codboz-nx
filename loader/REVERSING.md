# Marmalade `.s3e` ("XE3U") container

Recovered from `assets/boz.s3e` in the Call of Duty: Black Ops Zombies APK
(v1.0.11). Every claim here is verified by `s3e_loader_check.py`, and the whole
model is verified end to end by `run_boz.py`, which loads the image and executes
it under Unicorn until it reaches named import calls.

## Outer layer

`boz.s3e` is a raw **LZMA-alone** stream — 13-byte header (`5d 00 00 01 00`,
then an 8-byte little-endian output size), no container. 1,902,539 →
**4,550,559** bytes. Note that current liblzma *rejects* this alone-header
("Corrupt input data"); parse the 13 bytes yourself and drive the raw LZMA1
filter, as `lzma_alone()` does.

## Header — 19 little-endian words at offset 0

| Off | Value | Meaning |
|---|---|---|
| `+0x00` | `0x55334558` | magic `'XE3U'` |
| `+0x04` | `0x00042800` | unidentified (lands mid-function as an RVA) |
| `+0x08` | `0x010c000a` | version |
| `+0x0c` | `0x000043f3` | **file** offset of the import table |
| `+0x10` | `0x00035090` | points **into the middle of** the relocation list — do not use |
| `+0x14` | `0x00039483` | **file** offset of RVA 0 — the constant `K` |
| `+0x18` | `0x0041d970` | end of initialised data (RVA) |
| `+0x1c` | `0x004a7dc8` | bytes to allocate, including BSS |
| `+0x20` | `0x00456df3` | `= +0x18 + +0x14` |
| `+0x2c` / `+0x30` | `0x4c` / `0x43a7` | **file** offset and length of the ICF config text |
| `+0x34` | `0x4a000000` | **link base — the intended load address** |
| `+0x44` | `0x003dc000` | RVA of 391 Thumb pointers: one default implementation per import |
| `+0x24 +0x38 +0x3c +0x40` | | unidentified |

The header mixes **file offsets** with **RVAs**; confusing the two costs hours.

## The two constants

```
file_offset = RVA + K            K = 0x39483  (== header +0x14)
load the image AT 0x4a000000     (header +0x34)
```

`K` is pinned three ways: `+0x20 == +0x18 + +0x14` exactly; `K == +0x14`, i.e.
the metadata ends where the image begins; and 100% of relocation targets then
hold words inside `[link_base, link_base + image_size)`.

**The link base is not a placeholder.** Code reaches RVAs through absolute
`0x4a0000xx` values that no relocation entry covers, so loading anywhere else
faults immediately. Load at `0x4a000000` and relocation becomes a no-op.

## Layout

```
file 0x000000  header (19 words)
     0x00004c  ICF config text
     0x0043f3  import table: u32 pad, u32 name_bytes, u16 count(391), 391 names
     0x005d53  typed block chain (below)
     0x039483  RVA 0 -- start of the mapped image; ~3.4 MB Thumb-2 code, then data
     0x456df3  end of initialised data (RVA 0x41d970)
     0x456f9f  EOF
memory                        BSS to 0x4a7dc8 (566,360 bytes)
```

The image sits at an **odd** file offset, so word reads are unaligned.

## Typed block chain

Immediately after the import names, `{u32 tag, u32 bytes, u32 count}` + payload,
repeating until the image at `K`. Locate the first block by the identity
`bytes == 12 + 4*count`; the chain then ends exactly on `K`.

| tag | at | bytes | count | payload |
|---|---|---|---|---|
| 1 | `0x005d53` | 208,312 | 52,075 | relocation RVAs, u32 each |
| 2 | `0x038b0b` | 16 | 0 | empty |
| 4 | `0x038b1b` | 2,408 | 399 | symbol references, 6 bytes each |

**Relocations: 52,075, not 3,742.** Header `+0x10` points into the middle of
the tag-1 payload; reading from there yields a 3,742-entry slice whose "garbage
tail" is merely the list's real end. All 52,075 validate. Each entry is an RVA
whose word holds `link_base + target`; rebase it.

## Imports: an ARM ELF PLT

- **PLT0** (the resolver) is 20 bytes at RVA `0xb4`.
- **381 stubs**, *scattered through the code* rather than in one table, each:
  `add ip,pc,#A / add ip,ip,#B / ldr pc,[ip,#C]!` → slot `= stub + 8 + A + B + C`.
- **GOT**: RVA `0x411020..0x411610`, 381 contiguous slots, **every one
  pre-filled with `0x4a0000b4`** — a pointer back to PLT0.

**Filling those 381 GOT slots is the loader's import contract.** Header `+0x44`
(`0x3dc000`) is *not* the slot table; it is 391 Thumb pointers to per-import
default implementations.

### Naming the slots

The tag-4 block's 399 records are `{u16 0x41, u16 A, u16 import_index}`.
`import_index` covers 0..390 (all 391 present, 8 repeats). `A` is a **16-bit
offset within the GOT's own 64 KB page**:

```
got_slot_rva = (first_got_rva & ~0xFFFF) + A      /* 0x410000 + A here */
```

That names 378 of the 381 slots. **GOT order is linker order, not name order** —
slot 0 is `eglQueryString`, not `s3eMallocBase` — so this table is the only way
to map slots to names.

## Entry point

**RVA 0, in ARM (A32), not Thumb.** A stock C runtime stub: `ldr r0,[pc,#140] /
push {r4,lr} / sub sp,#8 / add r0,pc,r0 / bl 0x830 / tst sp,#4` (stack-alignment
self-test) `/ … / bl 0x3496fc`. No header field points at it; the entry is
implicitly the image base. The bulk of the code is Thumb-2, so **the binary is
mixed ARM/Thumb and any CPU backend needs interworking.**

## Float ABI

**softfp.** VFP for arithmetic, but arguments and returns travel in core
registers: `vmov r0, s15 / bx lr` returns a float in `r0`. The thunks in
`../gen/` marshal accordingly.

## Verified startup sequence

`run_boz.py` reaches, in order:

```
s3eDebugTraceLine("_IwMain")
s3eExtGetHash(hash, out_table, 0x50)   -- non-zero return means "present";
                                          the caller then calls table entries
                                          unconditionally, so fill them all
s3eMemoryGetInt / s3eMemorySetInt / s3eMallocBase / s3eReallocBase
s3eMemorySetUserMemMgr(mgr, 0x34c1e0, 0x34c1c4)  -- the game installs its own
s3eConfigGetString(...)
```

It then calls through its own allocator vtable, which needs the memory manager
to be genuinely wired up — the next piece of HLE work.

## Still unknown

- Header `+0x04`, `+0x24`, `+0x38`, `+0x3c`, `+0x40`; the tag-2 block.
- 3 of 381 GOT slots have no tag-4 record.
- 21 of the 399 tag-4 records point outside the GOT page window (probably data
  relocations rather than PLT entries).
