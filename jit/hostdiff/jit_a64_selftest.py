"""Exercise the raw AArch64 encodings used by ../jit.c under Unicorn.

This is intentionally independent of the Switch build. It catches emitter
mistakes (wrong register field, flag mask, struct offset) before an NRO has to
execute dynamically generated code on hardware.
"""
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X30
import struct


def mov32(out, rd, value):
    out.append(0x52800000 | ((value & 0xFFFF) << 5) | rd)
    if value >> 16:
        out.append(0x72A00000 | (((value >> 16) & 0xFFFF) << 5) | rd)


def rr(out, base, rd, rn, rm):
    out.append(base | (rm << 16) | (rn << 5) | rd)


def ldrw(out, rt, off):
    out.append(0xB9400000 | ((off >> 2) << 10) | rt)


def strw(out, rt, off):
    out.append(0xB9000000 | ((off >> 2) << 10) | rt)


def merge_nzcv(out, mask):
    out.append(0xD53B420C)                 # mrs x12,nzcv
    ldrw(out, 10, 64)                      # Guest.cpu.cpsr
    mov32(out, 11, mask)
    rr(out, 0x0A200000, 10, 10, 11)       # bic
    rr(out, 0x0A000000, 12, 12, 11)       # and
    rr(out, 0x2A000000, 10, 10, 12)       # orr
    strw(out, 10, 64)


def run(code, initial):
    code_addr, state = 0x100000, 0x200000
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(code_addr, 0x1000)
    uc.mem_map(state, 0x1000)
    uc.mem_write(code_addr, b"".join(struct.pack("<I", x) for x in code))
    for off, value in initial.items():
        uc.mem_write(state + off, struct.pack("<I", value))
    uc.reg_write(UC_ARM64_REG_X0, state)
    uc.reg_write(UC_ARM64_REG_X30, code_addr + 4 * len(code))
    uc.emu_start(code_addr, code_addr + 4 * len(code))
    return uc, state


def u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


code = []
mov32(code, 9, 0x12345678)
strw(code, 9, 4)                            # guest r1
ldrw(code, 9, 4)
mov32(code, 10, 1)
rr(code, 0x2B000000, 9, 9, 10)             # adds w9,w9,w10
merge_nzcv(code, 0xF0000000)
strw(code, 9, 8)                            # guest r2
mov32(code, 0, 2)
code.append(0xD65F03C0)
uc, state = run(code, {64: 0x000F0020})
assert u32(uc, state + 4) == 0x12345678
assert u32(uc, state + 8) == 0x12345679
assert u32(uc, state + 64) == 0x000F0020

code = []
mov32(code, 9, 0)
mov32(code, 10, 1)
rr(code, 0x6B000000, 9, 9, 10)             # subs 0,1
merge_nzcv(code, 0xF0000000)
strw(code, 9, 12)
code.append(0xD65F03C0)
uc, state = run(code, {64: 0x000F0020})
assert u32(uc, state + 12) == 0xFFFFFFFF
assert u32(uc, state + 64) == 0x800F0020     # N=1, Z/C/V=0

print("AArch64 emitter self-test passed")
