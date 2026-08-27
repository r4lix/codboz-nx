#!/usr/bin/env python3
"""Reader for the Marmalade "DTRZ" archive used by CoD:BOZ (.dz / .obb).

Layout, all little-endian:

    0x0000  'DTRZ'
    0x0004  u16 n_files
    0x0006  u16 n_groups
    0x0008  string table: n_files + n_groups NUL-terminated strings, the
            first empty. Entry i takes string i+1; group g takes string
            n_files+g. The two ranges overlap by one at index n_files.
    ...     n_files x 6-byte records {u16 group, u16 name_idx, u16 0xffff}
    ...     u16 version(1), u16 count(== n_files)
    ...     count x 16-byte entries {u32 offset, u32 size, u32 usize, u32 flags}
    ...     file data, stored back to back starting at entry[0].offset

Entries are contiguous: offset[i] + size[i] == offset[i+1]. Several files are
alternates of one logical asset (the four deadops-endgame-* variants share a
group id), which is how the game picks a resolution.

Usage:
    python dtrz.py <archive>              list
    python dtrz.py <archive> <name> [out] extract
"""
import os, struct, sys


class Dtrz:
    def __init__(self, path):
        self.path = path
        self.fh = open(path, "rb")
        head = self.fh.read(0x10000)
        if head[:4] != b"DTRZ":
            raise ValueError("not a DTRZ archive: %r" % head[:4])
        self.n_files, self.n_groups = struct.unpack_from("<HH", head, 4)

        # String table: n_files + n_groups strings, starting with an empty
        # placeholder. Entry i takes string i+1, and group g takes string
        # n_files+g -- the two ranges overlap by one at index n_files. Getting
        # the +1 wrong shifts every name by one and still parses cleanly, so
        # the giveaway is semantic: group "localisation" must contain the
        # language .dat files, "deadops-endgame" the four deadops variants.
        want = self.n_files + self.n_groups
        while True:
            strs, p, ok = [], 8, True
            while len(strs) < want:
                q = head.find(b"\x00", p)
                if q < 0:
                    ok = False
                    break
                strs.append(head[p:q].decode("latin1"))
                p = q + 1
            if ok:
                break
            head += self.fh.read(0x10000)      # rare: huge name table
        self.names = strs[1:1 + self.n_files]
        self.groups = strs[self.n_files:]

        # group records, then the entry table
        rec = p
        tbl = rec + 6 * self.n_files
        ver, cnt = struct.unpack_from("<HH", head, tbl)
        if ver != 1 or cnt != self.n_files:
            raise ValueError("unexpected entry table header %d/%d" % (ver, cnt))
        base = tbl + 4

        self.group_of = [struct.unpack_from("<H", head, rec + 6 * i)[0]
                         for i in range(self.n_files)]
        self.entries = [struct.unpack_from("<4I", head, base + 16 * i)
                        for i in range(self.n_files)]
        self.data_start = base + 16 * self.n_files
        self._off = {"strings": 8, "records": rec, "table": tbl, "data": self.data_start}

    def validate(self):
        """Return a list of problems; empty means the layout is fully consistent."""
        bad = []
        size = os.path.getsize(self.path)
        if self.entries[0][0] != self.data_start:
            bad.append("entry[0].offset 0x%x != end of table 0x%x"
                       % (self.entries[0][0], self.data_start))
        for i in range(self.n_files - 1):
            o, s = self.entries[i][0], self.entries[i][1]
            if o + s != self.entries[i + 1][0]:
                bad.append("entry %d: 0x%x+0x%x != 0x%x"
                           % (i, o, s, self.entries[i + 1][0]))
        o, s = self.entries[-1][:2]
        if o + s != size:
            bad.append("last entry ends 0x%x, file is 0x%x" % (o + s, size))
        for i, (o, s, u, fl) in enumerate(self.entries):
            if s != u:
                bad.append("entry %d is compressed (%d -> %d)" % (i, s, u))
        return bad

    def read(self, name):
        i = self.names.index(name)
        o, s, u, fl = self.entries[i]
        self.fh.seek(o)
        return self.fh.read(s)


def main():
    a = Dtrz(sys.argv[1])
    if len(sys.argv) >= 3:
        data = a.read(sys.argv[2])
        out = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(sys.argv[2])
        open(out, "wb").write(data)
        print("wrote %s (%d bytes)" % (out, len(data)))
        return
    print("%s: %d files, %d groups" % (os.path.basename(a.path),
                                       a.n_files, a.n_groups))
    print("offsets: %s" % ", ".join("%s=0x%x" % kv for kv in a._off.items()))
    bad = a.validate()
    print("validate: %s" % ("OK" if not bad else "%d problems" % len(bad)))
    for b in bad[:8]:
        print("   %s" % b)
    flags = {}
    for o, s, u, fl in a.entries:
        flags[fl] = flags.get(fl, 0) + 1
    print("flags: %s" % flags)
    print("\n%-4s %-10s %-10s %-6s %s" % ("idx", "offset", "size", "group", "name"))
    for i, (o, s, u, fl) in enumerate(a.entries[:12]):
        print("%-4d 0x%08x %-10d %-22s %s"
              % (i, o, s, a.groups[a.group_of[i]], a.names[i]))
    print("...")
    for i in range(a.n_files - 4, a.n_files):
        o, s, u, fl = a.entries[i]
        print("%-4d 0x%08x %-10d %-22s %s"
              % (i, o, s, a.groups[a.group_of[i]], a.names[i]))


if __name__ == "__main__":
    main()
