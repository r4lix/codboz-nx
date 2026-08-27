"""Diff the two harnesses' change logs for one watched word.

Both sides write 16-byte records {u64 index, u32 pc, u32 value} every time the
watched word changes. The index means different things on each side -- the C
harness counts its own steps, Unicorn counts writes -- so alignment is by
position in the change sequence, and only (pc, value) is compared.

The state hash in compare.py covers registers only, so a store to the wrong
address, or one that never happens, stays invisible until something loads the
word back. This says which write first went wrong.
"""
import struct, sys

LOAD_BASE = 0x4A000000


def load(p):
    b = open(p, "rb").read()
    return [struct.unpack_from("<QII", b, o) for o in range(0, len(b) - 15, 16)]


def rva(pc):
    d = (pc - LOAD_BASE) & 0xFFFFFFFF
    return "RVA %06x" % d if d < 0x500000 else "abs %08x" % pc


def show(tag, recs, k):
    for n in range(max(0, k - 4), min(len(recs), k + 4)):
        idx, pc, val = recs[n]
        print("    %s%-4d idx=%-12d pc=%08x %-12s value=%08x"
              % (">" if n == k else " ", n, idx, pc, rva(pc), val))


def main(pc_path, py_path):
    a, b = load(pc_path), load(py_path)
    print("interp: %d changes   unicorn: %d changes" % (len(a), len(b)))
    for k in range(min(len(a), len(b))):
        if a[k][1] != b[k][1] or a[k][2] != b[k][2]:
            print("\nFIRST DIVERGENT WRITE at change #%d" % k)
            kind = ("SITE (a different instruction wrote it)"
                    if a[k][1] != b[k][1] else "VALUE (same site, wrong value)")
            print("  kind   : %s" % kind)
            print("  interp : pc=%08x %s value=%08x  (step %d)"
                  % (a[k][1], rva(a[k][1]), a[k][2], a[k][0]))
            print("  unicorn: pc=%08x %s value=%08x"
                  % (b[k][1], rva(b[k][1]), b[k][2]))
            print("\n  interp changes:")
            show("i", a, k)
            print("  unicorn changes:")
            show("u", b, k)
            return 1
    if len(a) != len(b):
        n = min(len(a), len(b))
        print("\nagree over %d changes, then one side has %d more" % (n, abs(len(a) - len(b))))
        print("  extra on: %s" % ("interp" if len(a) > len(b) else "unicorn"))
        show("x", a if len(a) > len(b) else b, n)
        return 1
    print("\nno divergence: %d changes agree" % len(a))
    return 0


sys.exit(main(sys.argv[1], sys.argv[2]))
