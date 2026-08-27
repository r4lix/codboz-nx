# The `DTRZ` archive (`.dz` / `.obb`)

Decoded from `com.activision.boz/blackops_gles1.obb` (340,431,546 bytes).
Reader and validator: `dtrz.py`. Every field below is confirmed by
`Dtrz.validate()`, which passes with zero problems on this archive.

## Layout

All little-endian. Nothing is compressed and nothing is aligned — files are
stored back to back.

```
0x0000  'DTRZ'
0x0004  u16 n_files          190
0x0006  u16 n_groups         176
0x0008  string table         n_files + n_groups NUL-terminated strings
0x2079  n_files x 6 bytes    {u16 group, u16 name_idx, u16 0xffff}
0x24ed  u16 version (1), u16 count (== n_files)
0x24f1  count x 16 bytes     {u32 offset, u32 size, u32 usize, u32 flags}
0x30d1  file data
```

The table sizes are self-checking:

- `0x24f1 + 190*16 == 0x30d1` — the data begins immediately after the entry
  table, and `entry[0].offset` is exactly that.
- `offset[i] + size[i] == offset[i+1]` for every entry.
- `offset[189] + size[189]` == the file size exactly.
- `size == usize` for all 190 entries and `flags == 0x100` throughout, i.e.
  everything is stored, not compressed.

## The string table is off by one, and it parses fine either way

The table opens with an **empty string**, so:

```
name  of entry i  = strs[i + 1]
name  of group g  = strs[n_files + g]
```

The two ranges **overlap by one** at index `n_files`. Dropping the `+1` still
parses cleanly and produces 190 plausible-looking names — it just assigns every
one to the wrong entry. The tell is semantic, not structural:

| | wrong (no `+1`) | right |
|---|---|---|
| group `bootstrap` | `style.group.bin` | `bootstrap.group.bin` |
| group `deadops-endgame` | bootstrap + 3 variants | the 4 `deadops-endgame-*` variants |
| group `localisation` | `downloader.group.bin`, 2 languages | all 7 language files |

Confirmed against content: entry 0 is 1,976,339 bytes and contains the string
`iwui_style`; entry 1 is 765 bytes, contains `bootstrap`, and references
`bootstrap//iwui_style/style.group`. So entry 0 is `style.group.bin` and
entry 1 is the small `bootstrap.group.bin` manifest that points at it —
matching the `+1` mapping, not the naive one.

## Groups

`group` in each 6-byte record indexes the group-name range. Groups collect
alternates of one logical asset, which is how the game picks a variant:

```
0  style.group.bin                       bootstrap\iwui_style
1  bootstrap.group.bin                   bootstrap
2  deadops-endgame-1280x768.group.bin    deadops-endgame
3  deadops-endgame-ipad.group.bin        deadops-endgame
4  deadops-endgame-lowres.group.bin      deadops-endgame
5  deadops-endgame.group.bin             deadops-endgame
6  lv10_tropicalforest.group.bin         deadops-environments\lv10_tropicalforest
...
189 spanish.dat                          localisation
```

`name_idx` in the record always equals the entry index, and the third u16 is
always `0xffff`.

## Notes for the file layer

- The archive contains only `*.group.bin` and `*.dat`. The game opens
  **`blackops_hires.dz`** — that is the archive itself under a variant name
  (this copy is the `gles1` variant), not an entry inside it.
- It also opens **`data-sw/console.bin`**, which is not in this archive; it is
  a loose file that has to be served (or failed) separately.
- `blackops_gles1.dz.dat` alongside the obb is 4 bytes: `a4 f7 06 00`
  (456,100) — purpose unknown, not needed to read the archive.
- Contents are Marmalade `IwResGroup` binaries (they start with `0x3d` and
  carry the group name near the head); decoding *those* is a separate job.
