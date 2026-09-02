"""Disassemble a range of the loaded .s3e image by RVA.

The container is LZMA-alone compressed and is not an ELF, so objdump cannot open
it directly. This decompresses it once (cached), slices out the requested RVA
range, and feeds objdump a raw binary with the RVA supplied as the vma -- so
every address printed here is in the same RVA space as the profiler output, the
hook table and the logs.

Relocations are deliberately NOT applied: they only rewrite absolute data
pointers, and leaving them alone keeps this a pure view of the shipped code.

  python loader/disasm.py 36f140 128        # Thumb; the game is mostly Thumb
  python loader/disasm.py 2f7e64 96 arm
"""
import os, struct, subprocess, sys, tempfile, lzma

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S3E = os.path.join(ROOT, "boz.s3e")
CACHE = os.path.join(tempfile.gettempdir(), "boz_image.bin")
OBJDUMP = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump.exe"


def lzma_alone(raw):
    filt = [{"id": lzma.FILTER_LZMA1,
             "lc": raw[0] % 9, "lp": (raw[0] // 9) % 5, "pb": (raw[0] // 45),
             "dict_size": struct.unpack_from("<I", raw, 1)[0]}]
    return lzma.LZMADecompressor(format=lzma.FORMAT_RAW,
                                 filters=filt).decompress(
        raw[13:], struct.unpack_from("<Q", raw, 5)[0])


def image():
    """The decompressed code image, indexed by RVA. Cached across runs."""
    if os.path.exists(CACHE):
        return open(CACHE, "rb").read()
    f = lzma_alone(open(S3E, "rb").read())
    h = struct.unpack_from("<19I", f, 0)
    assert h[0] == 0x55334558, "bad magic"
    K, data_end, size = h[5], h[6], h[7]
    img = bytearray(size)
    img[0:data_end] = f[K:K + data_end]
    open(CACHE, "wb").write(img)
    return bytes(img)


def disasm(rva, length, mode="thumb"):
    blob = image()[rva:rva + length]
    if len(blob) < length:
        raise SystemExit("short read at %06x: got %d" % (rva, len(blob)))
    tmp = os.path.join(tempfile.gettempdir(), "boz_%06x.bin" % rva)
    open(tmp, "wb").write(blob)
    cmd = [OBJDUMP, "-D", "-b", "binary", "-m", "armv7", "-EL",
           "--adjust-vma=0x%x" % rva]
    if mode == "thumb":
        cmd += ["-M", "force-thumb"]
    cmd.append(tmp)
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    return "\n".join(ln for ln in out.splitlines()
                     if ln[:1].isspace() and ":" in ln)


if __name__ == "__main__":
    rva = int(sys.argv[1], 16)
    length = int(sys.argv[2], 0) if len(sys.argv) > 2 else 128
    print(disasm(rva, length, sys.argv[3] if len(sys.argv) > 3 else "thumb"))
