"""Find the first instruction where the interpreter truly diverges from Unicorn.

Both sides write {pc, state_hash} per instruction, recorded *before* it
retires. Unicorn boots this image clean, so a genuine mismatch localises an
interpreter bug to one instruction -- the class a decode-coverage check cannot
see, because the instruction decodes to the right group and only its effect is
wrong.

The two streams are not record-aligned: Unicorn does not fire UC_HOOK_CODE for
an instruction whose IT-block condition fails, while the C side records every
instruction it steps. Those show up as extra interpreter records that leave the
state hash unchanged. So this resynchronises rather than comparing index to
index, and only reports a divergence when re-alignment fails outright.
"""
import struct, sys

LOAD_BASE = 0x4A000000
WINDOW = 64          # how far ahead to hunt for a re-alignment
CONFIRM = 4          # records that must agree before believing a re-alignment
LOCAL_MAX = 64       # consecutive no-op interp records droppable without a resync


def load(p):
    b = open(p, "rb").read()
    n = len(b) // 8
    return struct.unpack("<%dI" % (2 * n), b[:8 * n]), n


def rva(pc):
    d = (pc - LOAD_BASE) & 0xFFFFFFFF
    return "RVA %06x" % d if d < 0x500000 else "abs %08x" % pc


def agree(a, i, b, j, k):
    return a[2 * (i + k)] == b[2 * (j + k)] and a[2 * (i + k) + 1] == b[2 * (j + k) + 1]


def confirm(a, na, i, b, nb, j):
    if i + CONFIRM > na or j + CONFIRM > nb:
        return False
    return all(agree(a, i, b, j, k) for k in range(CONFIRM))


def main(pa, pb):
    a, na = load(pa)
    b, nb = load(pb)
    print("interp: %d records   unicorn: %d records" % (na, nb))

    i = j = 0
    skips_a = skips_b = 0
    local = 0
    while i < na and j < nb:
        if agree(a, i, b, j, 0):
            i += 1
            j += 1
            local = 0
            continue

        # A condition-failed instruction in an IT block changes nothing and
        # Unicorn does not record it, so it appears as an extra interp record.
        # Each record carries the state *before* its instruction, so the extra
        # one's hash equals the record that FOLLOWS it, not the one before --
        # testing the predecessor only fires when the previous instruction was
        # also inert, which is why `itete ne` (two dead slots four records
        # apart) was reported as a true divergence.
        #
        # Clusters defeat any fixed-length confirmation window: the second skip
        # lands before CONFIRM records have matched. Handle them locally
        # instead -- drop one inert record when doing so lands exactly on the
        # unicorn record, which is a stronger check than confirming ahead and
        # cannot swallow a real divergence (a store leaves the register hash
        # unchanged too, so "inert" alone is not enough to drop on). Bounded so
        # nothing can be skipped forever.
        if (local < LOCAL_MAX and i + 1 < na
                and a[2 * i + 1] == a[2 * (i + 1) + 1]
                and agree(a, i + 1, b, j, 0)):
            local += 1
            skips_a += 1
            i += 1
            continue

        # Try to re-align: skip records on one side, cheapest first.
        best = None
        for d in range(1, WINDOW):
            if confirm(a, na, i + d, b, nb, j):
                best = ("interp", d)
                break
            if confirm(a, na, i, b, nb, j + d):
                best = ("unicorn", d)
                break
        if best is None:
            print("\nTRUE DIVERGENCE -- interp record %d, unicorn record %d" % (i, j))
            print("  interp : pc=%08x %s hash=%08x" % (a[2 * i], rva(a[2 * i]), a[2 * i + 1]))
            print("  unicorn: pc=%08x %s hash=%08x" % (b[2 * j], rva(b[2 * j]), b[2 * j + 1]))
            kind = ("CONTROL FLOW" if a[2 * i] != b[2 * j]
                    else "VALUE (pc agrees, state differs)")
            print("  kind   : %s" % kind)
            lo = max(0, i - 10)
            print("\n  last agreed instructions:")
            for k in range(lo, i):
                print("    %08x  %s" % (a[2 * k], rva(a[2 * k])))
            print("\n  interp continues:")
            for k in range(i, min(i + 6, na)):
                print("    %08x  %s  hash=%08x" % (a[2 * k], rva(a[2 * k]), a[2 * k + 1]))
            print("  unicorn continues:")
            for k in range(j, min(j + 6, nb)):
                print("    %08x  %s  hash=%08x" % (b[2 * k], rva(b[2 * k]), b[2 * k + 1]))
            return 1

        side, d = best
        if side == "interp":
            # Extra interpreter records. Harmless only if they changed nothing.
            for k in range(d):
                if a[2 * (i + k) + 1] != a[2 * i + 1]:
                    print("\nWARNING: skipped interp record %d (pc=%08x) CHANGED state"
                          % (i + k, a[2 * (i + k)]))
            skips_a += d
            i += d
        else:
            skips_b += d
            j += d

    print("\nno divergence: streams agree over %d instructions" % min(i, j))
    print("resyncs: %d interp-only records (condition-failed, state unchanged), "
          "%d unicorn-only" % (skips_a, skips_b))
    return 0


sys.exit(main(sys.argv[1], sys.argv[2]))
