"""List Thumb function boundaries in an RVA range, smallest first.

Finds entries by their prologue (push {...,lr} / stmdb sp!{...,lr}) and the
matching pop {...,pc}. Leaf functions with no push are missed by design -- the
point is to rank candidates for native replacement by size, and a routine small
enough to need no frame is found by eye from the profiler span anyway.

  python loader/funcs.py 5000 400
"""
import sys
from disasm import image

def funcs(lo, length):
    img = image()[lo:lo + length]
    out, start = [], None
    for i in range(0, len(img) - 1, 2):
        hw = img[i] | (img[i + 1] << 8)
        if (hw & 0xFE00) == 0xB400 and (hw & 0x0100):        # push {..., lr}
            if start is None:
                start = i
        elif (hw & 0xFE00) == 0xBC00 and (hw & 0x0100):      # pop {..., pc}
            if start is not None:
                out.append((lo + start, i + 2 - start))
                start = None
    return out

if __name__ == "__main__":
    lo = int(sys.argv[1], 16)
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x400
    fs = funcs(lo, n)
    print("%d functions with a frame in %06x..%06x" % (len(fs), lo, lo + n))
    for a, sz in sorted(fs, key=lambda t: t[1]):
        print("  %06x  %4d bytes  ~%d insns" % (a, sz, sz // 2))
