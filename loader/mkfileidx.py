"""Flatten the DTRZ archives into a plain index the C harness can serve.

Every file the game can open is a byte range of some real file on disk, so the
runtime never needs a DTRZ parser -- it needs {name -> archive, offset, size}.
This produces that table once, on the PC, and `jit/s3e_files.c` reads it.

Mirrors the mount order in run_boz.py (DATA_DIR then CACHE, first wins) so the
C file layer resolves exactly the same bytes the Unicorn reference does.

    .venv/Scripts/python.exe loader/mkfileidx.py [out_dir]

Writes <out_dir>/boz_files.idx and reports which archives to copy to the SD
card alongside it.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from dtrz import Dtrz

DATA_DIR = os.path.join(ROOT, "com.activision.boz")
CACHE = os.path.join(ROOT, ".cache")

MAGIC = b"BOZI"
VERSION = 1


def main(out_dir):
    archives = []          # basenames, in mount order
    index = {}             # lowered name -> (archive_idx, offset, size)

    for d in (DATA_DIR, CACHE):
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            if not (n.endswith(".dz") or n.endswith(".obb")):
                continue
            path = os.path.join(d, n)
            try:
                a = Dtrz(path)
            except Exception as e:
                print("  skip %s (%s)" % (n, e))
                continue
            ai = len(archives)
            # Store the path relative to ROOT so the host can read the
            # archives where they already live; the C side falls back to the
            # basename, which is the flat layout on the SD card.
            rel = os.path.relpath(path, ROOT).replace("\\", "/")
            archives.append((rel, path, a.n_files))
            for i, nm in enumerate(a.names):
                off, size = a.entries[i][0], a.entries[i][1]
                full = nm.replace("\\", "/").lower()
                base = nm.replace("\\", "/").rsplit("/", 1)[-1].lower()
                # setdefault: first mount wins, matching run_boz.py
                index.setdefault(full, (ai, off, size))
                index.setdefault(base, (ai, off, size))

    if not archives:
        print("no archives found under %s or %s" % (DATA_DIR, CACHE))
        return 1

    names = sorted(index)
    out = bytearray()
    out += MAGIC
    out += struct.pack("<III", VERSION, len(archives), len(names))
    for n, _p, _c in archives:
        b = n.encode("utf-8")
        out += struct.pack("<H", len(b)) + b
    for n in names:
        ai, off, size = index[n]
        b = n.encode("utf-8")
        out += struct.pack("<IIIH", ai, off, size, len(b)) + b

    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, "boz_files.idx")
    with open(dst, "wb") as f:
        f.write(out)

    print("archives mounted:")
    for n, p, c in archives:
        print("  %-28s %7d files  %10d bytes" % (n, c, os.path.getsize(p)))
    print("%d index entries (incl. basename aliases) -> %s (%d bytes)"
          % (len(names), dst, len(out)))
    print()
    print("copy to the SD card next to boz.s3e.unpacked:")
    print("  boz_files.idx")
    for n, _p, _c in archives:
        print("  %s" % n)
    for probe in ("console.bin", "blackops_loader.dz", "startup.group"):
        hit = index.get(probe)
        print("  probe %-20s %s" % (probe, "found" if hit else "MISSING"))
    return 0


sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else ROOT))
