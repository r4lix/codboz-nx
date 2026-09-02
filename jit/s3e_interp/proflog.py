"""Compare profile windows across builds in nxlog.txt.

Raw ms/300f is not comparable between windows: the scene changes as you play,
and a heavier scene costs more for reasons that have nothing to do with the
build. Two views fix that.

  - Exact pairs: windows from different builds with the SAME instruction count
    and draw count are the same work, so their times can be compared directly.
    These are the only unarguable numbers here.
  - ms per 100M instructions: interpreter cost per unit of work, which lets
    unequal scenes be compared. Note this deliberately hides gains that come
    from REMOVING instructions -- a native hook that deletes a hot routine
    leaves this figure flat while making the game faster. Read it alongside the
    instruction counts, never instead of them.

Clock speed is not recorded anywhere in the log, so builds run at different
clocks are silently incomparable. r54 was measured at stock clocks and the
rest overclocked; that is why it is excluded rather than shown.

  python jit/s3e_interp/proflog.py [nxlog.txt] [min_draws]
"""
import os, re, sys
from collections import defaultdict

EXCLUDE = {"pcprof-r54"}          # measured at a different clock


def windows(path):
    build, cur, out = None, {}, []
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"interpreter test - (\S+)", ln)
        if m:
            build = m.group(1)
            continue
        m = re.match(r"\s*\[prof \] (\d+) ms/300f", ln)
        if m:
            cur = {"build": build, "ms": int(m.group(1))}
            continue
        m = re.search(r"glDrawElements\s+\d+%\s+(\d+) calls", ln)
        if m and cur:
            cur["de"] = int(m.group(1))
            continue
        m = re.match(r"\s*\[pcprof\] (\d+)M guest instructions", ln)
        if m and cur:
            cur["Minsn"] = int(m.group(1))
            out.append(cur)
            cur = {}
    return [w for w in out if w.get("build") not in EXCLUDE
            and "Minsn" in w and "de" in w]


def main(path, min_draws):
    ws = windows(path)
    same = defaultdict(list)
    for w in ws:
        same[(w["Minsn"], w["de"])].append(w)

    print("Identical workloads across builds (same instructions AND draws):")
    any_pair = False
    for key, v in sorted(same.items()):
        by = defaultdict(list)
        for w in v:
            by[w["build"]].append(w["ms"])
        if len(by) < 2:
            continue
        any_pair = True
        print("  %4dM insn %6d draws:  %s" % (key[0], key[1], "  ".join(
            "%s %s" % (b, "/".join(str(m) for m in sorted(ms)))
            for b, ms in sorted(by.items()))))
    if not any_pair:
        print("  (none -- play the same scene on both builds)")

    print("\nPer-build, draws > %d:" % min_draws)
    by = defaultdict(list)
    for w in ws:
        if w["de"] > min_draws:
            by[w["build"]].append(w)
    for b in sorted(by):
        v = by[b]
        norm = sorted(w["ms"] / w["Minsn"] * 100 for w in v)
        insn = sorted(w["Minsn"] for w in v)
        print("  %-12s n=%2d  median %6.0f ms/100M insn   median %4dM insn"
              % (b, len(v), norm[len(norm) // 2], insn[len(insn) // 2]))


if __name__ == "__main__":
    p = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "nxlog.txt")
    main(p, int(sys.argv[2]) if len(sys.argv) > 2 else 20000)
