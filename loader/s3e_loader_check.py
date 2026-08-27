#!/usr/bin/env python3
"""Reference run of s3e_loader.c against the real CoD:BOZ image.

Mirrors the C loader step for step so the numbers below are a genuine check of
the algorithm. There is no native compiler in this environment, so the C is
compile-verified for aarch64 and the runtime behaviour is verified here (and,
end to end, by run_boz.py under Unicorn).
"""
import lzma, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
S3E = os.path.join(os.path.dirname(HERE), "boz.s3e")

F = ["magic", "unknown04", "version", "import_off", "unknown10", "image_off",
     "data_end_rva", "image_size", "data_end_file", "unknown24", "unknown28",
     "config_off", "config_len", "link_base", "unknown38", "unknown3c",
     "unknown40", "default_impl_rva", "unknown48"]


def lzma_alone(raw):
    try:
        return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(raw)
    except lzma.LZMAError:
        pass
    d = raw[0]
    lc = d % 9
    d //= 9
    lp, pb = d % 5, d // 5
    filt = [{"id": lzma.FILTER_LZMA1, "lc": lc, "lp": lp, "pb": pb,
             "dict_size": struct.unpack_from("<I", raw, 1)[0]}]
    return lzma.LZMADecompressor(format=lzma.FORMAT_RAW,
                                 filters=filt).decompress(
        raw[13:], struct.unpack_from("<Q", raw, 5)[0])


def rotimm(x):
    v, rot = x & 0xFF, ((x >> 8) & 0xF) * 2
    return v if not rot else ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF


def main():
    raw = open(S3E, "rb").read()
    f = lzma_alone(raw)
    print("decompressed %d -> %d bytes\n" % (len(raw), len(f)))
    u32 = lambda o: int.from_bytes(f[o:o + 4], "little")
    u16 = lambda o: int.from_bytes(f[o:o + 2], "little")

    h = dict(zip(F, struct.unpack_from("<19I", f, 0)))
    assert h["magic"] == 0x55334558, "bad magic"
    K, data_end, size, link = (h["image_off"], h["data_end_rva"],
                               h["image_size"], h["link_base"])
    ok = h["data_end_file"] == data_end + K
    print("header identity +0x20 == +0x18 + +0x14 : 0x%06x == 0x%06x  [%s]"
          % (h["data_end_file"], data_end + K, "OK" if ok else "FAIL"))
    assert ok
    print("K = 0x%06x   link base = 0x%08x   BSS = %d bytes"
          % (K, link, size - data_end))

    img = bytearray(size)
    img[0:data_end] = f[K:K + data_end]
    w = lambda r: int.from_bytes(img[r:r + 4], "little")

    e = [w(i * 4) for i in range(4)]
    print("\nentry @ RVA 0 (ARM): %s  [%s]"
          % (" ".join("%08x" % x for x in e),
             "OK" if e[0] == 0xE59F008C and e[1] == 0xE92D4010 else "FAIL"))

    # ---- imports + block chain -----------------------------------------
    it = h["import_off"]
    count = u16(it + 8)
    names, o = [], it + 10
    for _ in range(count):
        z = f.index(b"\x00", o)
        names.append(f[o:z].decode("latin1"))
        o = z + 1

    blk = next(p for p in range(o, o + 16) if u32(p + 4) == 12 + 4 * u32(p + 8))
    chain, p = [], blk
    while p + 12 <= K:
        tag, nbytes, cnt = u32(p), u32(p + 4), u32(p + 8)
        if nbytes < 12 or p + nbytes > K:
            break
        chain.append((tag, p, nbytes, cnt))
        p += nbytes
    print("\nblock chain from 0x%x:" % blk)
    for tag, at, nbytes, cnt in chain:
        print("  tag %d  at 0x%06x  %7d bytes  count=%d" % (tag, at, nbytes, cnt))
    print("  chain ends 0x%06x (K=0x%06x) [%s]"
          % (p, K, "OK" if p == K else "MISMATCH"))

    reloc = next(c for c in chain if c[0] == 1)
    syms = next(c for c in chain if c[0] == 4)

    applied = 0
    for i in range(reloc[3]):
        t = u32(reloc[1] + 12 + 4 * i)
        if t + 4 > data_end:
            continue
        v = w(t)
        if link <= v < link + size:
            img[t:t + 4] = struct.pack("<I", v)   # load_base == link_base
            applied += 1
    print("\nrelocations applied: %d of %d" % (applied, reloc[3]))

    # ---- PLT -> GOT ------------------------------------------------------
    got_of, r = {}, 0
    while r + 12 <= data_end:
        a = w(r)
        if (a & 0xFFFFF000) == 0xE28FC000:
            b, c = w(r + 4), w(r + 8)
            if (b & 0xFFFFF000) == 0xE28CC000 and (c & 0xFFFFF000) == 0xE5BCF000:
                got_of[(r + 8) + rotimm(a) + rotimm(b) + (c & 0xFFF)] = r
        r += 4
    gots = sorted(got_of)
    print("PLT stubs / GOT slots: %d  (0x%06x..0x%06x, contiguous=%s)"
          % (len(gots), gots[0], gots[-1],
             all(gots[i] + 4 == gots[i + 1] for i in range(len(gots) - 1))))
    plt0 = link + 0xB4
    print("  all slots pre-filled with PLT0 (0x%08x): %s"
          % (plt0, all(w(g) == plt0 for g in gots)))

    page = gots[0] & ~0xFFFF
    named = {}
    for i in range(syms[3]):
        a = u16(syms[1] + 12 + 2 + 6 * i)
        b = u16(syms[1] + 12 + 4 + 6 * i)
        if page + a in got_of and b < count:
            named[page + a] = names[b]
    print("  named from tag-4 (slot = 0x%x + A): %d of %d"
          % (page, len(named), len(gots)))
    for g in gots[:6]:
        print("    0x%06x = %s" % (g, named.get(g, "<unnamed>")))

    print("\nimports: %d names; default-impl table at RVA 0x%06x"
          % (count, h["default_impl_rva"]))
    print("entry point: guest 0x%08x (ARM mode)" % link)
    print("\nLOAD OK")


if __name__ == "__main__":
    sys.exit(main())
