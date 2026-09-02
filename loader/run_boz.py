#!/usr/bin/env python3
"""Execute the loaded .s3e image under Unicorn and trace its imports.

Imports are reached through a standard ARM ELF PLT. Each GOT slot is bound to
its own stub address in an unused page; a code hook there turns every call into
a logged trap whose GOT slot -- and therefore whose import name -- is encoded in
the address.

Run with the venv interpreter:
  .venv/Scripts/python.exe loader/run_boz.py
"""
import collections, lzma, os, struct, sys, time

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from unicorn import *
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
S3E = os.path.join(os.path.dirname(HERE), "boz.s3e")

STACK_BASE, STACK_SIZE = 0x20000000, 1 << 20
# Layout: stack 0x20000000, surface 0x40000000, image 0x4a000000..~0x4a4a8000,
# heap 0x60000000 (room up to the stub page at 0xf0000000), stubs 0xf0000000.
HEAP_BASE = 0x60000000
HEAP_SIZE = int(os.environ.get("BOZ_HEAP_MB", "1024")) << 20
STUB_BASE, STUB_SIZE = 0xF0000000, 0x1000
INSN_LIMIT = int(os.environ.get("BOZ_INSNS", 200_000_000))

PAGE = 0x1000
align = lambda n: (n + PAGE - 1) & ~(PAGE - 1)

SCREEN_W, SCREEN_H = 480, 320
# The game asks for a pixel type via s3eSurfaceSetup (0x422 here). Bytes per
# pixel is configurable so both readings can be tested against the output.
SURF_BASE = 0x40000000
SURF_BPP = int(os.environ.get("BOZ_BPP", "2"))
SURF_PIXTYPE = int(os.environ.get("BOZ_PIXTYPE", "0x422"), 0)
# The game writes past the nominal end of the surface, so over-allocate and
# report the real extent it touches rather than guessing the geometry.
SURF_FRAME = SCREEN_W * SCREEN_H * SURF_BPP
SURF_BYTES = align(SURF_FRAME) + (16 << 20)


def lzma_alone(raw):
    """Newer liblzma rejects this file's alone-header, so drive LZMA1 raw."""
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


def load_image():
    f = lzma_alone(open(S3E, "rb").read())
    u32 = lambda o: int.from_bytes(f[o:o + 4], "little")
    u16 = lambda o: int.from_bytes(f[o:o + 2], "little")
    h = struct.unpack_from("<19I", f, 0)
    assert h[0] == 0x55334558, "bad magic"
    K, data_end, size, link = h[5], h[6], h[7], h[13]
    assert h[8] == data_end + K, "layout identity failed"

    # The image's link base is its intended load address: code reaches RVAs
    # through absolute 0x4a0000xx values that no relocation covers.
    load_base = link

    img = bytearray(size)
    img[0:data_end] = f[K:K + data_end]
    w = lambda r: int.from_bytes(img[r:r + 4], "little")

    # ---- import names -------------------------------------------------
    it = h[3]
    count = u16(it + 8)
    names, o = [], it + 10
    for _ in range(count):
        e = f.index(b"\x00", o)
        names.append(f[o:e].decode("latin1"))
        o = e + 1

    # ---- typed block chain, from the end of the names to the image -----
    # {u32 tag, u32 block_bytes, u32 count} + payload.
    # tag 1 = relocation RVAs (u32 each)
    # tag 4 = symbol references, 6-byte records {u16 0x41, u16 A, u16 import}
    blocks = {}
    p = next(q for q in range(o, o + 16) if u32(q + 4) == 12 + 4 * u32(q + 8))
    while p + 12 <= K:
        tag, nbytes, cnt = u32(p), u32(p + 4), u32(p + 8)
        if nbytes < 12 or p + nbytes > K:
            break
        blocks[tag] = (p + 12, cnt)
        p += nbytes

    rel, nrel = blocks[1]
    applied = 0
    lo, hi = link, link + size
    for i in range(nrel):
        t = u32(rel + 4 * i)
        if t + 4 > data_end:
            continue
        v = w(t)
        if lo <= v < hi:
            img[t:t + 4] = struct.pack("<I", v - link + load_base)
            applied += 1

    # ---- PLT stubs -> GOT slots ---------------------------------------
    # add ip,pc,#A / add ip,ip,#B / ldr pc,[ip,#C]!  =>  slot = stub+8+A+B+C
    got_of, r = {}, 0
    while r + 12 <= data_end:
        a = w(r)
        if (a & 0xFFFFF000) == 0xE28FC000:
            b, c = w(r + 4), w(r + 8)
            if (b & 0xFFFFF000) == 0xE28CC000 and (c & 0xFFFFF000) == 0xE5BCF000:
                got_of[(r + 8) + rotimm(a) + rotimm(b) + (c & 0xFFF)] = r
        r += 4
    gots = sorted(got_of)

    # ---- name each GOT slot from the tag-4 block ----------------------
    # Field A is a 16-bit offset within the GOT's own 64 KB page.
    sym, nsym = blocks[4]
    page = gots[0] & ~0xFFFF
    slot_name = {}
    for i in range(nsym):
        a = u16(sym + 2 + 6 * i)
        b = u16(sym + 4 + 6 * i)
        if page + a in got_of and b < count:
            slot_name[page + a] = names[b]

    global ICF_TEXT
    ICF_TEXT = f[h[11]:h[11] + h[12]].decode("latin1", "replace")
    print("relocations applied : %d of %d" % (applied, nrel))
    print("GOT slots           : %d (0x%06x..0x%06x)" % (len(gots), gots[0], gots[-1]))
    print("named from tag-4    : %d of %d" % (len(slot_name), len(gots)))
    return img, load_base, gots, [slot_name.get(g, "<slot 0x%06x>" % g) for g in gots]


ICF_TEXT = ""
img, LOAD_BASE, gots, slot_names = load_image()


def parse_icf(text):
    """Marmalade ICF: [Section] headers, key=value lines, {COND} guards."""
    cfg, sect, cond_ok = {}, "", True
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("{") and line.endswith("}"):
            c = line[1:-1].strip()
            cond_ok = (not c) or ("ANDROID" in c.upper())
            continue
        if line.startswith("[") and line.endswith("]"):
            sect = line[1:-1].strip()
            continue
        if "=" in line and cond_ok:
            k, v = line.split("=", 1)
            cfg[(sect.lower(), k.strip().lower())] = v.strip().strip('"')
    return cfg


CONFIG_H = os.path.join(os.path.dirname(HERE), "jit", "s3e_config.h")


def load_config_table(path):
    """The overrides the C harnesses answer with, read from their own header.

    This reference and jit/hostdiff/host_main.c must answer the SDK
    identically or the differential reports divergences that are really just
    config drift, so the table is parsed rather than copied. Entries are
    X("section", "key", "value"), one per line -- see jit/s3e_config.h.
    """
    import re

    def macro_body(text, name):
        """The continuation lines of `#define <name>(X)`, and nothing else.

        Bounded deliberately: S3E_CONFIG_PARKED sits right below the enabled
        table and holds keys that must stay unanswered. Scanning to end of
        file would pull them in and put this reference back out of step with
        the C harnesses -- the exact drift the shared header exists to stop.
        """
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if not line.startswith("#define %s(" % name):
                continue
            body = []
            while line.endswith("\\") and i + 1 < len(lines):
                i += 1
                line = lines[i]
                body.append(line)
            return "\n".join(body)
        raise SystemExit("no %s in %s" % (name, path))

    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    pairs = re.findall(r'X\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)',
                       macro_body(text, "S3E_CONFIG_TABLE"))
    if not pairs:
        raise SystemExit("no S3E_CONFIG_TABLE entries parsed from %s" % path)
    parked = re.findall(r'X\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)',
                        macro_body(text, "S3E_CONFIG_PARKED"))
    print("cfg: %d keys enabled, %d parked (jit/s3e_config.h)"
          % (len(pairs), len(parked)))
    return {(s.lower(), k.lower()): v for s, k, v in pairs}


# The blob embedded in the .s3e is the *system* ICF: it carries Windows/WP8
# settings that break this game if served wholesale (it faults during startup).
# Default to answering "not set" -- the game then uses its own defaults, which
# is what worked -- and layer the deliberate overrides on top.
ICF = parse_icf(ICF_TEXT) if os.environ.get("BOZ_ICF_FULL") == "1" else {}
_overrides = load_config_table(CONFIG_H)
ICF.update(_overrides)
# [RESMANAGER] ResBuildStyle is added once the archives are mounted -- it has
# to name a pack that is really there. See the file layer below.
print("icf: %d keys from %d bytes (%d overrides from s3e_config.h)"
      % (len(ICF), len(ICF_TEXT), len(_overrides)))

uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
uc.mem_map(LOAD_BASE, align(len(img)))
uc.mem_write(LOAD_BASE, bytes(img))
for b, s in ((STACK_BASE, STACK_SIZE), (HEAP_BASE, HEAP_SIZE),
             (STUB_BASE, STUB_SIZE), (SURF_BASE, SURF_BYTES)):
    uc.mem_map(b, s)

uc.mem_write(STUB_BASE, b"\x1e\xff\x2f\xe1" * (STUB_SIZE // 4))  # ARM `bx lr`
for i, g in enumerate(gots):
    uc.mem_write(LOAD_BASE + g, struct.pack("<I", STUB_BASE + 4 * i))

uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)          # ARMv7 boots with VFP trapped
uc.reg_write(UC_ARM_REG_SP, STACK_BASE + STACK_SIZE - 16)

heap_ptr, heap_sizes = HEAP_BASE, {}
free_lists = collections.defaultdict(list)
trace, counts, ticks = [], collections.Counter(), [0]
SND_CHANNELS = 16                      # must match SND_CHANNELS in host_main.c
_snd_playing = [0] * SND_CHANNELS
_snd_next = [0]
recent = collections.deque(maxlen=25)
callbacks = {}
pending = []


def gstr(addr, limit=256):
    if not addr:
        return "<null>"
    out = b""
    try:
        while len(out) < limit:
            c = uc.mem_read(addr + len(out), 1)
            if c == b"\x00":
                break
            out += c
    except UcError:
        return "<unreadable 0x%08x>" % addr
    return out.decode("latin1", "replace")


def safe_write(addr, data):
    """Guest pointers handed to us can be bogus; never let that kill the run."""
    try:
        uc.mem_write(addr, data)
        return True
    except UcError:
        return False


_oom = [0]


def do_free(p):
    n = heap_sizes.get(p)
    if n:
        free_lists[n].append(p)


def do_malloc(n):
    global heap_ptr
    n = (n + 15) & ~15
    # BOZ_TRACE compares against the C interpreter, whose harness never
    # recycles; reusing a freed block here would look like a CPU divergence.
    bucket = None if os.environ.get("BOZ_TRACE") else free_lists.get(n)
    if bucket:
        return bucket.pop()
    if heap_ptr + n > HEAP_BASE + HEAP_SIZE:
        # A bump allocator with a no-op free never reclaims, so exhaustion is
        # self-inflicted. The game does not NULL-check: it runs the C++ ctor on
        # the result and faults writing the vtable at offset 0.
        _oom[0] += 1
        if _oom[0] <= 3:
            print("   [OOM  ] malloc(%d) failed, %d MB used"
                  % (n, (heap_ptr - HEAP_BASE) >> 20))
        return 0
    p, heap_ptr = heap_ptr, heap_ptr + n
    heap_sizes[p] = n
    return p


_strcache = {}


def alloc_str(text):
    """Return a guest pointer to a NUL-terminated copy of `text`."""
    if text not in _strcache:
        b = text.encode("latin1") + b"\x00"
        p = do_malloc(len(b))
        uc.mem_write(p, b)
        _strcache[text] = p
    return _strcache[text]


# s3eDeviceGetString returns const char*; callers dereference it without a
# NULL check, so every query has to yield a real string.
DEVICE_STRINGS = {}
DEVICE_DEFAULT = "unknown"

# Property queries. Filled in once the log shows which ids the game asks for.
# Every value here has to match hle_gl_getint / hle_surface_getint /
# hle_ptr_getint in jit/hostdiff/host_main.c exactly. A differential compares
# two CPUs, so the SDK underneath them must be the same SDK -- one differing
# property answer re-routes the game and every instruction after it is a false
# divergence.
QUERY = {
    # S3E_GL_VERSION is packed major/minor -- 0x110 = 1.1, which is the path
    # this build (blackops_gles1) wants. The 0x11 this used to answer is 17,
    # below every version test in the image, so it silently selected the
    # lowest-capability path everywhere.
    ("s3eGLGetInt", 0): int(os.environ.get("BOZ_GLVER", "0x110"), 0),
    ("s3eSurfaceGetInt", 0): SCREEN_W,
    ("s3eSurfaceGetInt", 1): SCREEN_H,
    ("s3eSurfaceGetInt", 2): SCREEN_W * SURF_BPP,
    ("s3eSurfaceGetInt", 3): SURF_PIXTYPE,
    ("s3eSurfaceGetInt", 4): SCREEN_W,
    ("s3eSurfaceGetInt", 5): SCREEN_H,
    ("s3eSurfaceGetInt", 6): SCREEN_W * SURF_BPP,   # device pitch
    # Screen orientation in quarter turns, not a flag: the game subtracts it
    # from its own stored orientation and indexes a 4-entry blit jump table.
    ("s3eSurfaceGetInt", 11): int(os.environ.get("BOZ_ORIENT", "0")),
    ("s3ePointerGetInt", 0): 1,     # AVAILABLE
    ("s3ePointerGetInt", 2): 2,     # TYPE = STYLUS (touchscreen)
    ("s3ePointerGetInt", 3): 2,     # STYLUS_TYPE = FINGER
    ("s3ePointerGetInt", 4): 0,     # MULTI_TOUCH_AVAILABLE = no
}

# Where a synthetic tap lands, and what s3ePointerGetX/Y report until then.
TAP_X, TAP_Y = SCREEN_W // 2, SCREEN_H // 2
# 0 disables it. A deep differential runs with the tap off on both sides: the
# two harnesses model the press/release sequence differently (this one fires
# the callbacks straight from the driver loop, the C one runs a state machine
# off s3ePointerUpdate), so with input in play they are not executing the same
# program and nothing downstream is comparable.
TAP_FRAME = int(os.environ.get("BOZ_TAP", "4"))

# ------------------------------------------------------------------- files
# The game parses the DTRZ archive itself, so the file layer only has to serve
# raw bytes. Search order: the app data dir, then the APK's assets/.
import zipfile
from dtrz import Dtrz

ROOT = os.path.dirname(HERE)
DATA_DIR = os.path.join(ROOT, "com.activision.boz")
APK = os.path.join(ROOT, "CODBOZ.apk")
CACHE = os.path.join(ROOT, ".cache")
FILE_HANDLE_BASE = 0xF1000000

os.makedirs(CACHE, exist_ok=True)
_apk = zipfile.ZipFile(APK) if os.path.isfile(APK) else None
_apk_names = {n.rsplit("/", 1)[-1]: n for n in (_apk.namelist() if _apk else [])}

# Pull the loader archive out of the APK once; it holds console.bin and the
# bootstrap groups, which the on-disk obb does not.
for _want in ("blackops_loader.dz",):
    _dst = os.path.join(CACHE, _want)
    if not os.path.isfile(_dst) and _apk and _want in _apk_names:
        with _apk.open(_apk_names[_want]) as _s, open(_dst, "wb") as _d:
            _d.write(_s.read())

def _idx_mount_order():
    """The archives boz_files.idx was built from, in its own mount order.

    Scanning a directory picks by filename order, which is a trap once more
    than one pack is present: blackops_atitc.dz sorts first and would silently
    win here while mkfileidx.py deliberately gave the C harness a different
    one. The two would then serve different bytes and answer ResBuildStyle
    differently, and the differential would report that as a CPU divergence.
    So follow the index when it exists; it *is* the deliberate choice.
    """
    path = os.path.join(ROOT, "boz_files.idx")
    try:
        with open(path, "rb") as fh:
            head = fh.read(16)
            if head[:4] != b"BOZI":
                return []
            _ver, na, _ne = struct.unpack_from("<III", head, 4)
            out = []
            for _ in range(na):
                (ln,) = struct.unpack("<H", fh.read(2))
                out.append(fh.read(ln).decode("utf-8").replace("\\", "/"))
            return out
    except (OSError, struct.error, UnicodeDecodeError):
        return []


# Every resolvable file is a byte range of some real file on disk, whether it
# is loose, an archive entry, or an extracted APK member. The index's order
# comes first; anything else on disk is appended, so nothing that used to
# resolve stops resolving.
_scanned = []
for _d in (DATA_DIR, CACHE):
    for _n in sorted(os.listdir(_d)) if os.path.isdir(_d) else []:
        if _n.endswith(".dz") or _n.endswith(".obb"):
            _scanned.append(os.path.join(_d, _n))
_idx_order = _idx_mount_order()
_ordered = [os.path.join(ROOT, *_r.split("/")) for _r in _idx_order]
_ordered = [_p for _p in _ordered if os.path.isfile(_p)]
_ordered += [_p for _p in _scanned if _p not in _ordered]

_mounts, _index = [], {}
for _p in _ordered:
    _n = os.path.basename(_p)
    try:
        _a = Dtrz(_p)
    except Exception:
        continue
    _mounts.append((_n, _a))
    for _i, _nm in enumerate(_a.names):
        _o, _s = _a.entries[_i][0], _a.entries[_i][1]
        _index.setdefault(_nm.replace("\\", "/").lower(), (_p, _o, _s))
        _index.setdefault(_nm.rsplit("\\", 1)[-1].lower(), (_p, _o, _s))
print("mounted: %s" % ", ".join("%s(%d)" % (n, a.n_files) for n, a in _mounts))

# Mirrors s3e_vfs_build_style() in jit/s3e_files.c: the first mounted texture
# pack wins, because file resolution is first-mount-wins too. Naming a build
# the archives do not hold would leave the resource manager with no textures at
# all, which is worse than the software fallback it takes when the key is unset.
_PACKS = (("blackops_etc", "etc"), ("blackops_dxt", "dxt"),
          ("blackops_atitc", "atitc"), ("blackops_gles1", "gles1"))
BUILD_STYLE = "gles1"
_seen_packs = []
for _n, _a in _mounts:
    for _needle, _style in _PACKS:
        if _needle in _n.lower():
            _seen_packs.append((_n, _style))
if _seen_packs:
    BUILD_STYLE = _seen_packs[0][1]
if len(_seen_packs) > 1 and not _idx_order:
    # No index to follow, so the winner came from directory order -- the trap
    # mkfileidx.py documents. If it disagrees with the pack the C harness
    # mounted, the two serve different bytes and answer ResBuildStyle
    # differently, and the differential reports that as a CPU divergence.
    print("WARNING: %d texture packs and no boz_files.idx to choose between "
          "them (%s); '%s' wins by directory order"
          % (len(_seen_packs), ", ".join(n for n, _ in _seen_packs),
             _seen_packs[0][0]))
elif len(_seen_packs) > 1:
    print("packs: %s (ignored: %s)"
          % (_seen_packs[0][0], ", ".join(n for n, _ in _seen_packs[1:])))
ICF[("resmanager", "resbuildstyle")] = BUILD_STYLE
print("cfg: ResBuildStyle=%s" % BUILD_STYLE)

_files, _next_handle, _file_err = {}, [FILE_HANDLE_BASE], [0]


SAVE_DIR = os.path.join(CACHE, "save")


def vfs_save_path(name):
    n = name.replace("\\", "/").lstrip("./")
    return os.path.join(SAVE_DIR, *n.split("/"))


def vfs_resolve(name):
    """Guest path -> (real path, offset, size), or None."""
    n = name.replace("\\", "/").lstrip("./")
    base = n.rsplit("/", 1)[-1]

    for cand in (vfs_save_path(name),
                 os.path.join(DATA_DIR, *n.split("/")),
                 os.path.join(CACHE, base)):
        if os.path.isfile(cand):
            return (cand, 0, os.path.getsize(cand))
    for key in (n.lower(), base.lower()):
        if key in _index:
            return _index[key]
    if _apk and base in _apk_names:                 # extract on demand
        dst = os.path.join(CACHE, base)
        with _apk.open(_apk_names[base]) as s, open(dst, "wb") as d:
            d.write(s.read())
        return (dst, 0, os.path.getsize(dst))
    # a .dz the game names but we do not have is the main archive under a
    # variant name (this copy is the gles1 build)
    if base.endswith(".dz"):
        for cand in sorted(os.listdir(DATA_DIR)):
            if cand.endswith(".obb") or cand.endswith(".dz"):
                p = os.path.join(DATA_DIR, cand)
                return (p, 0, os.path.getsize(p))
    return None


def _new_handle(fh, base, size, name, writable=False):
    h = _next_handle[0]
    _next_handle[0] += 4
    _files[h] = {"fh": fh, "base": base, "size": size, "pos": 0,
                 "name": name, "w": writable}
    _file_err[0] = 0
    return h


def vfs_open(name, mode="rb"):
    # Saves go to a sandbox under .cache/save so the game can persist state
    # without touching the real asset files.
    if any(c in mode for c in "wa+"):
        p = vfs_save_path(name)
        os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
        fh = open(p, "w+b" if "w" in mode or not os.path.isfile(p) else "r+b")
        if "a" in mode:
            fh.seek(0, 2)
        return _new_handle(fh, 0, os.path.getsize(p), name, writable=True)

    r = vfs_resolve(name)
    if not r:
        _file_err[0] = 3                            # S3E_FILE_ERR_NOT_FOUND
        return 0
    path, off, size = r
    return _new_handle(open(path, "rb"), off, size, name)


def vfs_write(h, data):
    f = _files.get(h)
    if not f or not f.get("w"):
        return 0
    f["fh"].seek(f["base"] + f["pos"])
    f["fh"].write(data)
    f["pos"] += len(data)
    f["size"] = max(f["size"], f["pos"])
    return len(data)


def vfs_read(h, n):
    f = _files.get(h)
    if not f:
        return b""
    n = max(0, min(n, f["size"] - f["pos"]))
    f["fh"].seek(f["base"] + f["pos"])
    out = f["fh"].read(n)
    f["pos"] += len(out)
    return out


# ----------------------------------------------------------------- surface
# s3eSurfacePtr must hand back a real framebuffer; the game blits into it and
# calls s3eSurfaceShow to present. Snapshot each present as a PNG.
import zlib

OUT_DIR = os.path.join(HERE, "out")
_frames = [0]
_prev, _best, _dupes = [None], [0], [0]


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def present():
    buf = bytes(uc.mem_read(SURF_BASE, SURF_FRAME))
    os.makedirs(OUT_DIR, exist_ok=True)
    # Most presents repeat the previous image; encoding those costs 150k Python
    # iterations each for nothing. Skip duplicates and keep the richest frame.
    if buf == _prev[0]:
        _dupes[0] += 1
        return
    _prev[0] = buf
    distinct = len({buf[i] | (buf[i + 1] << 8) for i in range(0, len(buf) - 1, 128)})
    if distinct > _best[0]:
        _best[0] = distinct
        open(os.path.join(OUT_DIR, "best.raw"), "wb").write(buf)
        print("   [surf ] frame %d: %d distinct values -> best.raw"
              % (_frames[0], distinct))
    rgb = bytearray(SCREEN_W * SCREEN_H * 3)
    if SURF_BPP == 2:                                   # RGB565
        for i in range(SCREEN_W * SCREEN_H):
            v = buf[2 * i] | (buf[2 * i + 1] << 8)
            rgb[3 * i] = ((v >> 11) & 0x1F) * 255 // 31
            rgb[3 * i + 1] = ((v >> 5) & 0x3F) * 255 // 63
            rgb[3 * i + 2] = (v & 0x1F) * 255 // 31
    else:                                               # ARGB8888 (B,G,R,A)
        for i in range(SCREEN_W * SCREEN_H):
            rgb[3 * i] = buf[4 * i + 2]
            rgb[3 * i + 1] = buf[4 * i + 1]
            rgb[3 * i + 2] = buf[4 * i]
    os.makedirs(OUT_DIR, exist_ok=True)
    if _frames[0] == 0:
        open(os.path.join(OUT_DIR, "frame000.raw"), "wb").write(buf)
    p = os.path.join(OUT_DIR, "frame%03d.png" % _frames[0])
    write_png(p, SCREEN_W, SCREEN_H, bytes(rgb))
    nz = sum(1 for b in buf if b)
    print("   [frame] %s  (%d/%d bytes non-zero)" % (os.path.basename(p), nz, len(buf)))
    _frames[0] += 1


EXT_STUB = STUB_BASE + 0xF00   # generic "extension function" -> returns 0


def on_stub(uc, address, size, _):
    # Resuming after an emu_stop restarts at the stub itself, which would run
    # the handler twice -- and s3eSurfaceShow is not idempotent.
    if _resume_skip[0] == address:
        _resume_skip[0] = 0
        return
    i = (address - STUB_BASE) // 4
    if i >= len(slot_names):
        counts["<ext-table stub>"] += 1
        uc.reg_write(UC_ARM_REG_R0, 0)
        return
    name = slot_names[i]
    counts[name] += 1
    if counts[name] == 1:
        print("   [first] %s" % name)
    r = [uc.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1,
                                  UC_ARM_REG_R2, UC_ARM_REG_R3)]
    if len(trace) < 300:
        trace.append((name, r))
    recent.append((name, r))
    ret = 0

    if name == "s3eMallocBase":
        ret = do_malloc(r[0])
    elif name == "s3eReallocBase":
        ret = do_malloc(r[1])
        if ret and r[0]:
            old = heap_sizes.get(r[0], 0)
            if old:
                uc.mem_write(ret, bytes(uc.mem_read(r[0], min(old, r[1]))))
    elif name in ("s3eDebugTraceLine", "s3eDebugOutputString", "s3eDebugPrint",
                  "s3eDebugAssertShow", "s3eDebugErrorShow"):
        parts = [gstr(x) for x in r[:3] if x]
        print("   [guest] %s: %s" % (name, " | ".join(parts) or "<no text>"))
    # ---- file layer (Marmalade argument orders) ------------------------
    elif name == "s3eFileOpen":                # (name, mode) -> s3eFile*
        fn = gstr(r[0])
        md = gstr(r[1])
        ret = vfs_open(fn, md)
        print("   [file ] open(%r, %r) -> %s"
              % (fn, md, "0x%08x" % ret if ret else "FAIL"))
    elif name == "s3eFileCheckExists":         # (name) -> s3eBool
        fn = gstr(r[0])
        ret = 1 if vfs_resolve(fn) else 0
        print("   [file ] exists(%r) -> %d" % (fn, ret))
    elif name == "s3eFileRead":                # (buf, elem, count, file) -> elems
        data = vfs_read(r[3], r[1] * r[2])
        if data and not safe_write(r[0], data):
            data = b""
        ret = len(data) // r[1] if r[1] else 0
    elif name == "s3eFileWrite":           # (buf, elem, count, file) -> elems
        n = r[1] * r[2]
        wrote = vfs_write(r[3], bytes(uc.mem_read(r[0], n))) if n else 0
        ret = wrote // r[1] if r[1] else 0
    elif name == "s3eFileSeek":                # (file, offset, origin)
        f = _files.get(r[0])
        if f:
            off = struct.unpack("<i", struct.pack("<I", r[1]))[0]
            f["pos"] = max(0, min({0: off, 1: f["pos"] + off,
                                   2: f["size"] + off}.get(r[2], off), f["size"]))
            ret = 0
        else:
            ret = 1
    elif name == "s3eFileTell":
        ret = _files.get(r[0], {}).get("pos", 0)
    elif name == "s3eFileGetSize":
        ret = _files.get(r[0], {}).get("size", 0)
    elif name == "s3eFileClose":
        f = _files.pop(r[0], None)
        if f and f["fh"]:
            f["fh"].close()
        ret = 0
    elif name == "s3eFileGetError":
        ret = _file_err[0]
    elif name == "s3eFileGetChar":
        d = vfs_read(r[0], 1)
        ret = d[0] if d else 0xFFFFFFFF
    elif name == "s3eFileDelete":
        p2 = vfs_save_path(gstr(r[0]))
        if os.path.isfile(p2):
            os.remove(p2)
        ret = 0
    elif name == "s3eFileMakeDirectory":
        os.makedirs(vfs_save_path(gstr(r[0])), exist_ok=True)
        ret = 0
    elif name == "s3eFileFlush":
        f2 = _files.get(r[0])
        if f2 and f2.get("w"):
            f2["fh"].flush()
        ret = 0
    elif name == "s3eFileGetFileInt":
        ret = _files.get(r[0], {}).get("size", 0)
    elif name == "s3eDeviceGetString":
        s = DEVICE_STRINGS.get(r[0], DEVICE_DEFAULT)
        print("   [dev  ] GetString(0x%x) -> %r" % (r[0], s))
        ret = alloc_str(s)
    elif name == "s3eExtGetHash":
        # (hash, out_table, bytes). Non-zero return means "extension present";
        # the caller then calls table entries unconditionally, so they must all
        # be valid. Point every one at a stub that just returns 0.
        ok = all(safe_write(r[1] + 4 * k, struct.pack("<I", EXT_STUB))
                 for k in range(r[2] // 4))
        ret = 1 if ok else 0
    elif name.endswith("Register") or name.endswith("UnRegister"):
        # s3eXxxRegister(cbid, fn, userData): r0 = event id, r1 = guest fn.
        if name.endswith("UnRegister"):
            callbacks.pop((name[:-10], r[0]), None)
        else:
            callbacks[(name[:-8], r[0])] = (r[1], r[2])
            print("   [cb   ] %-22s id=%-3d fn=0x%08x user=0x%08x"
                  % (name, r[0], r[1], r[2]))
    elif name == "s3eSurfaceSetup":
        print("   [surf ] %s(r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x)"
              % (name, r[0], r[1], r[2], r[3]))
    elif name == "s3eSurfacePtr":
        ret = SURF_BASE
    elif name == "s3eSurfaceShow":
        _shows[0] += 1
        # Same FNV-1a over the same bytes as surf_hash() in host_main.c, at the
        # same cadence, so the two runs' presented pixels can be compared
        # directly without diffing images. If these agree all the way to the
        # window there is nothing wrong with the CPU up to that point.
        if _shows[0] <= 4 or _shows[0] % 25 == 0:
            _sb = bytes(uc.mem_read(SURF_BASE, SURF_FRAME))
            _sh = 2166136261
            for _c in _sb:
                _sh = ((_sh ^ _c) * 16777619) & 0xFFFFFFFF
            print("   [surf ] present #%d hash=%08x" % (_shows[0], _sh))
        present()
        if TRACE_AT and _shows[0] == TRACE_AT:
            _stop_after[0] = 1
    elif name == "s3eGLGetInt":
        ret = QUERY.get(("s3eGLGetInt", r[0]), 0)
        print("   [query] s3eGLGetInt(0x%x) -> %d" % (r[0], ret))
    elif name == "s3eMemoryGetInt":
        # Mirrors hle_memory_getint in host_main.c. 0 is the one answer known to
        # make the game declare the device out of memory, so it is never the
        # fallback.
        used = heap_ptr - HEAP_BASE
        avail = HEAP_SIZE - used
        ret = {0: HEAP_SIZE, 1: used, 2: avail, 3: avail,
               4: avail, 5: used}.get(r[0], avail)
    elif name == "s3eSoundGetInt":
        # Mirrors hle_sound_getint. Property 0 is the channel count; answering 0
        # leaves the mixer unbuilt and the game later makes a virtual call
        # through the resulting NULL vtable.
        ret = {0: SND_CHANNELS, 1: 1, 2: 44100, 3: 1}.get(r[0], 1)
    elif name == "s3eSoundGetFreeChannel":
        for _k in range(SND_CHANNELS):
            _ch = (_snd_next[0] + _k) % SND_CHANNELS
            if not _snd_playing[_ch]:
                _snd_next[0] = (_ch + 1) % SND_CHANNELS
                ret = _ch
                break
        else:
            ret = 0
    elif name == "s3eSoundChannelPlay":
        if r[0] < SND_CHANNELS:
            _snd_playing[r[0]] = 1
        ret = 0
    elif name == "s3eSoundChannelStop":
        if r[0] < SND_CHANNELS:
            _snd_playing[r[0]] = 0
        ret = 0
    elif name == "s3eSoundChannelGetInt":
        ret = _snd_playing[r[0]] if (r[1] == 0 and r[0] < SND_CHANNELS) else 0
    elif name == "s3eAudioGetInt":
        ret = {0: 1, 1: 0, 2: 256}.get(r[0], 1)
    elif name in ("s3eSoundSetInt", "s3eSoundChannelSetInt",
                  "s3eSoundChannelPause", "s3eSoundChannelResume",
                  "s3eAudioSetInt", "s3eAudioPlay", "s3eAudioPlayFromBuffer",
                  "s3eAudioStop", "s3eAudioPause", "s3eAudioResume",
                  "s3eAudioIsPlaying"):
        ret = 0
    elif name in ("s3ePointerGetX", "s3ePointerGetTouchX"):
        ret = TAP_X
    elif name in ("s3ePointerGetY", "s3ePointerGetTouchY"):
        ret = TAP_Y
    elif name in ("s3eSurfaceGetInt", "s3eDeviceGetInt", "s3ePointerGetInt",
                  "s3eMemoryGetInt"):
        ret = QUERY.get((name, r[0]), 0)
        if name in ("s3eSurfaceGetInt", "s3eDeviceGetInt"):
            print("   [query] %s(0x%x) -> %d" % (name, r[0], ret))
    elif name in ("s3eConfigGetString", "s3eConfigGetInt"):
        # (section, key, out). 0 == S3E_RESULT_SUCCESS.
        v = ICF.get((gstr(r[0]).lower(), gstr(r[1]).lower()))
        if v is None:
            ret = 1
        elif name == "s3eConfigGetInt":
            try:
                ret = 0 if safe_write(r[2], struct.pack("<i", int(v, 0))) else 1
            except ValueError:
                ret = 1
        else:
            ret = 0 if safe_write(
                r[2], v.encode("latin1")[:255] + bytes(1)) else 1
    elif name == "s3eDeviceYield":
        # Marmalade pumps events here. Unicorn cannot re-enter emu_start from
        # inside a hook, so stop and let the driver loop deliver callbacks.
        if pending:
            uc.emu_stop()
    elif name in ("s3eTimerGetMs", "s3eTimerGetUST"):
        ticks[0] += 16
        ret = ticks[0]
    uc.reg_write(UC_ARM_REG_R0, ret)
    if _stop_after[0]:
        _stop_after[0] = 0
        _resume_skip[0] = address
        uc.emu_stop()


def on_bad_mem(uc, access, address, size, value, _):
    print("\n!! unmapped %s at 0x%08x pc=0x%08x"
          % ({UC_MEM_READ_UNMAPPED: "read", UC_MEM_WRITE_UNMAPPED: "write",
              UC_MEM_FETCH_UNMAPPED: "fetch"}.get(access, str(access)),
             address, uc.reg_read(UC_ARM_REG_PC)))
    return False


blocks_seen = collections.deque(maxlen=24)

# BOZ_TRACE_AT=<n>: open the trace window at the n'th s3eSurfaceShow instead of
# at the entry point. The two harnesses cannot be aligned by instruction index
# at this depth -- Unicorn does not fire UC_HOOK_CODE for an IT-block
# instruction whose condition fails and the C interpreter counts every step it
# takes, so the indices drift apart by a growing amount -- but both count
# s3eSurfaceShow calls the same way, natively, so it makes an exact landmark.
#
# Counting *shows*, not frames: _frames[0] below only advances on a frame whose
# pixels changed, and the C side counts every call. The first 175 presents of
# this image are pixel-identical, so the two counters are nowhere near each
# other.
TRACE_AT = int(os.environ.get("BOZ_TRACE_AT", "0"))
# BOZ_TRACE_PC=<addr> + BOZ_TRACE_NTH=<n>: open the window the n'th time that
# address executes. A present is too coarse an anchor once a memory watch has
# named the call that goes wrong; this puts the window straight on it, with the
# anchored instruction as record 0.
TRACE_PC = int(os.environ.get("BOZ_TRACE_PC", "0"), 0)
TRACE_NTH = int(os.environ.get("BOZ_TRACE_NTH", "1"))
DEEP = TRACE_AT or TRACE_PC
_pc_hits = [0]
_pc_resume = [0]
_shows = [0]
_stop_after = [0]      # emu_stop once the current stub handler has finished
_resume_skip = [0]     # stub address to pass through untouched on resume

# Unicorn runs code hooks in registration order, and the tracing hook has to
# come first: the C side writes its record *before* dispatching an HLE, so a
# record taken after on_stub had already rewritten r0 would disagree at every
# import call. An anchored window cannot register the tracing hook up front --
# a Python callback per instruction is exactly what the fast-forward has to
# avoid -- so the handles are kept and the hooks are re-registered behind it.
_code_hooks = []


def _add_code_hook(fn, begin, end):
    _code_hooks.append([fn, begin, end,
                        uc.hook_add(UC_HOOK_CODE, fn, begin=begin, end=end)])


def _trace_hook_first():
    for e in _code_hooks:
        uc.hook_del(e[3])
    uc.hook_add(UC_HOOK_CODE, _on_insn)
    for e in _code_hooks:
        e[3] = uc.hook_add(UC_HOOK_CODE, e[0], begin=e[1], end=e[2])
    # Adding an unranged code hook does NOT invalidate blocks Unicorn has
    # already translated, and by this depth essentially the whole hot path is
    # cached -- without this the hook fires on about one instruction in a
    # thousand and the "window" silently spans a hundred million instructions.
    uc.ctl_flush_tb()

# BOZ_TRACE=<file>: one 8-byte record per instruction, {pc, state hash}, in the
# same format jit/hostdiff writes. UC_HOOK_CODE fires *before* the instruction
# retires, which is the convention the C side records with too. Unicorn is the
# oracle here: it boots this image clean, so the first differing record is a bug
# in the interpreter, not in the game.
_TRACE_PATH = os.environ.get("BOZ_TRACE")
if _TRACE_PATH:
    _tf = open(_TRACE_PATH, "wb")
    _tregs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
              UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
              UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
              UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR]
    _FLAGS = 0xF00F0020          # N Z C V, GE[3:0], T -- what the C side hashes
    # r0-r14, cpsr, d0-d31, fpscr -- read in one batch per instruction.
    # D registers, not S: the upper bank d16-d31 has no single-precision view,
    # and this image uses it heavily, so hashing only s0-s31 would miss half
    # the float state.
    _tbatch = _tregs + [UC_ARM_REG_CPSR] + \
        [UC_ARM_REG_D0 + k for k in range(32)] + [UC_ARM_REG_FPSCR]
    _tbuf = bytearray()
    _tn = [0]
    _tlimit = int(os.environ.get("BOZ_TRACE_MAX", "2000000"))
    _skip_stub = [1 if TRACE_AT else 0]

    _full = os.environ.get("BOZ_FULL")
    _fs, _fc = (int(x) for x in _full.split(":")) if _full else (1 << 62, 0)

    def _on_insn(u, address, size, _):
        if _tn[0] >= _tlimit:
            u.emu_stop()
            return
        # The window is armed from inside the s3eSurfaceShow stub handler, so
        # the first instruction after the resume is that stub's own `bx lr`.
        # The C side never sees it -- its HLE replaces the stub outright, and
        # the step that dispatched it is the one the window opened on -- so
        # recording it here would offset the whole window by one record.
        if _skip_stub[0] and STUB_BASE <= address < STUB_BASE + STUB_SIZE:
            _skip_stub[0] = 0
            return
        if _fs <= _tn[0] < _fs + _fc:
            cp = u.reg_read(UC_ARM_REG_CPSR)
            print("[%d] pc=%08x %-5s cpsr=%08x" %
                  (_tn[0], address, "Thumb" if cp & 0x20 else "ARM", cp))
            vals = [u.reg_read(r) for r in _tregs]
            for k, v in enumerate(vals):
                print(" r%-2d=%08x" % (k, v), end="\n" if k % 4 == 3 else "")
            print()
            # VFP too: the state hash covers only core registers, so a wrong
            # double stays invisible until it is moved into r0/r1.
            _s = [u.reg_read(UC_ARM_REG_S0 + k) for k in range(32)]
            for k in range(16):
                print(" d%-2d=%08x%08x" % (k, _s[2 * k + 1], _s[2 * k]),
                      end="\n" if k % 2 else "")
            print("fpscr=%08x" % u.reg_read(UC_ARM_REG_FPSCR))
        # One batched read of the whole architectural state. VFP is in the hash
        # now: without it a wrong double is invisible until it reaches a core
        # register, which put the first observable divergence thousands of
        # instructions after the instruction that actually caused it.
        vals = u.reg_read_batch(_tbatch)
        h = 2166136261
        for k in range(15):                       # r0-r14
            v = vals[k]
            for b in range(4):
                h ^= (v >> (8 * b)) & 0xFF
                h = (h * 16777619) & 0xFFFFFFFF
        for v in (vals[15] & _FLAGS,):            # cpsr, masked
            for b in range(4):
                h ^= (v >> (8 * b)) & 0xFF
                h = (h * 16777619) & 0xFFFFFFFF
        for k in range(16, 48):                   # d0-d31, low word then high
            d = vals[k]
            for v in (d & 0xFFFFFFFF, (d >> 32) & 0xFFFFFFFF):
                for b in range(4):
                    h ^= (v >> (8 * b)) & 0xFF
                    h = (h * 16777619) & 0xFFFFFFFF
        # Only FPSCR's NZCV: the rounding-mode and cumulative-exception bits
        # are not modelled, and would diverge without meaning anything.
        for v in (vals[48] & 0xF0000000,):
            for b in range(4):
                h ^= (v >> (8 * b)) & 0xFF
                h = (h * 16777619) & 0xFFFFFFFF
        _tbuf.extend(struct.pack("<II", address, h))
        _tn[0] += 1
        if len(_tbuf) >= (1 << 20):
            _tf.write(_tbuf)
            del _tbuf[:]

    def _trace_flush():
        if _tbuf:
            _tf.write(_tbuf)
            del _tbuf[:]
        _tf.flush()

    if not DEEP:
        uc.hook_add(UC_HOOK_CODE, _on_insn)
    import atexit
    atexit.register(lambda: (_trace_flush(), _tf.close()))

_add_code_hook(on_stub, STUB_BASE, STUB_BASE + STUB_SIZE - 1)
uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_bad_mem)

# BOZ_WATCH=<guest addr>: log every change of one 32-bit word as
# {seq, pc, value}, the same 16-byte record the C harness writes. The state
# hash covers registers only, so a store to the wrong address is invisible
# until something loads the word back -- diffing the two change logs names the
# store that did it.
WATCH = int(os.environ.get("BOZ_WATCH", "0"), 0)
if WATCH:
    _wf = open("watch_py.bin", "wb")
    _wn = [0]
    _wlast = [struct.unpack("<I", bytes(uc.mem_read(WATCH, 4)))[0]]
    print("watching %08x (initially %08x) -> watch_py.bin" % (WATCH, _wlast[0]))

    def _on_watch(u, access, address, size, value, _):
        # UC_HOOK_MEM_WRITE fires before the store lands, and a byte or
        # halfword write only carries part of the word, so splice rather than
        # taking `value` as the new word.
        b = bytearray(u.mem_read(WATCH, 4))
        for k in range(size):
            off = address + k - WATCH
            if 0 <= off < 4:
                b[off] = (value >> (8 * k)) & 0xFF
        new = struct.unpack("<I", bytes(b))[0]
        if new != _wlast[0]:
            _wlast[0] = new
            _wf.write(struct.pack("<QII", _wn[0], u.reg_read(UC_ARM_REG_PC), new))
            _wn[0] += 1

    uc.hook_add(UC_HOOK_MEM_WRITE, _on_watch, begin=WATCH, end=WATCH + 3)
    import atexit
    atexit.register(lambda: (_wf.flush(), _wf.close(),
                             print("watch: %d changes to %08x" % (_wn[0], WATCH))))
# An unranged block hook is a Python call per basic block -- around one per ten
# instructions. That is affordable for a 50M bring-up run and completely
# unaffordable for a fast-forward of several hundred million, which is the only
# reason a deep window is reachable at all. It only feeds an end-of-run
# diagnostic, so drop it for those runs.
if not DEEP:
    uc.hook_add(UC_HOOK_BLOCK, lambda u, a, s, _: blocks_seen.append((a, s)))


def _on_anchor_pc(u, address, size, _):
    if _pc_resume[0] == address:           # the resumed pass over the anchor
        _pc_resume[0] = 0
        return
    _pc_hits[0] += 1
    if _pc_hits[0] == TRACE_NTH:
        _pc_resume[0] = address
        u.emu_stop()


if TRACE_PC:
    _add_code_hook(_on_anchor_pc, TRACE_PC, TRACE_PC)

def hle(rva, label, fn):
    """Replace a guest function outright: run fn, then return to LR.

    The app routes every allocation through a manager object it builds itself.
    That object's three function pointers are never populated here (its
    initialiser depends on startup we do not run), so its wrappers dereference
    NULL. A port replaces the guest allocator with the host one regardless, so
    intercept the three wrappers directly.
    """
    def cb(uc, address, size, _):
        counts["[hle] " + label] += 1
        fn(uc)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    a = LOAD_BASE + rva
    _add_code_hook(cb, a, a)


def _mgr_malloc(uc):
    uc.reg_write(UC_ARM_REG_R0, do_malloc(uc.reg_read(UC_ARM_REG_R0)))


def _mgr_realloc(uc):
    old, n = uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)
    p = do_malloc(n)
    if p and old:
        keep = min(heap_sizes.get(old, 0), n)
        if keep:
            uc.mem_write(p, bytes(uc.mem_read(old, keep)))
        do_free(old)
    uc.reg_write(UC_ARM_REG_R0, p)


def _mgr_free(uc):
    do_free(uc.reg_read(UC_ARM_REG_R0))
    uc.reg_write(UC_ARM_REG_R0, 0)


hle(0x34C1A8, "mgr malloc", _mgr_malloc)
hle(0x34C1E0, "mgr realloc", _mgr_realloc)
hle(0x34C1C4, "mgr free", _mgr_free)


def watch(rva, label, nwords=3, reg=UC_ARM_REG_R0):
    """Print a register and what it points at, each time an RVA executes."""
    def cb(uc, address, size, _):
        v = uc.reg_read(reg)
        try:
            got = " ".join("%08x" % x for x in struct.unpack(
                "<%dI" % nwords, uc.mem_read(v, 4 * nwords))) if v else "<null>"
        except UcError:
            got = "<unmapped>"
        print("   [watch] %-18s r=%08x -> %s" % (label, v, got))
    a = LOAD_BASE + rva
    uc.hook_add(UC_HOOK_CODE, cb, begin=a, end=a)


print("\nstarting at 0x%08x (ARM)\n" % LOAD_BASE)
CB_RET = STUB_BASE + 0xF80          # landing pad: callback returns here
SAVED = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
         UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
         UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
         UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_CPSR]


def call_guest(fn, system_data, user_data, label=""):
    """Call a guest callback and return, restoring the interrupted state."""
    saved = [(reg, uc.reg_read(reg)) for reg in SAVED]
    uc.reg_write(UC_ARM_REG_R0, system_data)
    uc.reg_write(UC_ARM_REG_R1, user_data)
    uc.reg_write(UC_ARM_REG_LR, CB_RET)
    ok = True
    try:
        uc.emu_start(fn, CB_RET & ~1, count=20_000_000)
    except UcError as e:
        ok = False
        print("   [cb   ] %s faulted: %s (pc=0x%08x)"
              % (label, e, uc.reg_read(UC_ARM_REG_PC)))
    for reg, v in saved:
        uc.reg_write(reg, v)
    return ok


def guest_alloc_struct(words):
    p = do_malloc(4 * len(words))
    for i, v in enumerate(words):
        uc.mem_write(p + 4 * i, struct.pack("<i", v))
    return p


err = None
executed = 0
pc = LOAD_BASE
SLICE = 25_000_000
tapped = [False]


def _resume_pc():
    """Where to restart: Unicorn wants the Thumb bit set in the start address."""
    p = uc.reg_read(UC_ARM_REG_PC)
    return (p | 1) if (uc.reg_read(UC_ARM_REG_CPSR) & 0x20) else p


if DEEP:
    # Two phases. The fast-forward runs with no per-instruction hook and no
    # instruction count -- a count limit is itself implemented as a per-
    # instruction hook, and the whole reason a window this deep is affordable
    # is that nothing runs per instruction. A wall-clock timeout is the safety
    # net instead.
    timeout_us = int(float(os.environ.get("BOZ_DEEP_TIMEOUT", "1800")) * 1e6)
    print("\n[deep ] fast-forwarding to %s (no hooks), then tracing %d "
          "instructions"
          % ("s3eSurfaceShow #%d" % TRACE_AT if TRACE_AT else
             "hit #%d of pc=0x%08x" % (TRACE_NTH, TRACE_PC), _tlimit))
    t0 = time.time()
    start = pc
    while True:
        try:
            uc.emu_start(start, LOAD_BASE + len(img), timeout=timeout_us)
        except UcError as e:
            err = e
            break
        if _resume_skip[0] or _pc_resume[0]:    # the anchor stopped us
            break
        nxt = _resume_pc()
        if nxt == start or not nxt or (nxt & ~1) >= LOAD_BASE + len(img):
            print("[deep ] stopped before the anchor at pc=0x%08x (shows=%d, "
                  "hits=%d)" % (nxt, _shows[0], _pc_hits[0]))
            break
        start = nxt                            # a yield or a timeout: continue
    print("[deep ] fast-forward: %d shows, %d anchor hits in %.1fs, pc=0x%08x"
          % (_shows[0], _pc_hits[0], time.time() - t0,
             uc.reg_read(UC_ARM_REG_PC)))

    reached = _shows[0] >= TRACE_AT if TRACE_AT else _pc_hits[0] >= TRACE_NTH
    if reached and err is None:
        _trace_hook_first()                    # flushes the TB cache for us
        t0 = time.time()
        try:
            uc.emu_start(_resume_pc(), LOAD_BASE + len(img), timeout=timeout_us)
        except UcError as e:
            err = e
        print("[deep ] window: %d records in %.1fs" % (_tn[0], time.time() - t0))
    else:
        print("[deep ] anchor never reached -- no window written")
    _trace_flush()
    executed = INSN_LIMIT                      # skip the ordinary driver loop

while executed < INSN_LIMIT:
    cpsr = uc.reg_read(UC_ARM_REG_CPSR)
    start = pc | 1 if (cpsr & 0x20) else pc
    try:
        uc.emu_start(start, LOAD_BASE + len(img), count=min(SLICE, INSN_LIMIT - executed))
    except UcError as e:
        err = e
        break
    executed += SLICE
    pc = uc.reg_read(UC_ARM_REG_PC)

    # Once the game is presenting steadily, synthesise a screen tap: a title
    # screen waiting for input would otherwise repeat one frame forever.
    # Armed on s3eSurfaceShow calls, the same counter the C harness arms on.
    if (not tapped[0] and TAP_FRAME and _shows[0] >= TAP_FRAME
            and ("s3ePointer", 0) in callbacks):
        tapped[0] = True
        fn, ud = callbacks[("s3ePointer", 0)]
        for pressed in (1, 0):
            ev = guest_alloc_struct([0, pressed, SCREEN_W // 2, SCREEN_H // 2])
            print("   [cb   ] firing s3ePointer id=0 pressed=%d" % pressed)
            call_guest(fn, ev, ud, "pointer")
        pending.append("tap")

    while pending:
        pending.pop()

    if pc == 0 or pc >= LOAD_BASE + len(img):
        break

pc, cpsr = uc.reg_read(UC_ARM_REG_PC), uc.reg_read(UC_ARM_REG_CPSR)
print("\nstopped: %s" % (err or "instruction limit / clean exit"))
print("pc=0x%08x (RVA 0x%06x) %s" % (pc, pc - LOAD_BASE,
                                     "Thumb" if cpsr & 0x20 else "ARM"))
print("last blocks:", " ".join("%06x" % (a - LOAD_BASE) for a, _ in
                               list(blocks_seen)[-8:]))

print("\n--- last %d import calls before the stop ---" % len(recent))
for name, r in recent:
    print("  %-30s r0=%08x r1=%08x r2=%08x r3=%08x" % (name, *r))

print("\n--- first %d import calls ---" % min(len(trace), 45))
for name, r in trace[:45]:
    print("  %-30s r0=%08x r1=%08x r2=%08x r3=%08x" % (name, *r))

print("\n--- %d distinct imports called ---" % len(counts))
for k, v in counts.most_common(35):
    print("  %-32s %d" % (k, v))
print("\nheap used: %d KB" % ((heap_ptr - HEAP_BASE) // 1024))
