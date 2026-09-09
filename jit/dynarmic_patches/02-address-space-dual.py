"""Make dynarmic's arm64 AddressSpace use a dual mapping on Horizon.

Upstream holds an oaknut::CodeBlock -- one address that is writable and
executable at once -- and asks the OS to flip permissions on it when W^X is
required. Horizon cannot do that: RWX is refused outright, and the only way to
get executable memory is two aliases of the same pages at two addresses.

Almost nothing has to change, because dynarmic already threads a write pointer
and an execute pointer separately: every CodeGenerator here is built as
`{w, x}`, and oaknut's set_xptr converts an execute address back to its write
address via `m_wmem + (p - m_xmem)`. On every other platform those two happen to
be equal, which is why upstream can pass `mem.ptr()` twice. Supplying two
genuinely different bases is the case the code was already written for.

The type is swapped behind an alias rather than by #ifdef-ing each use, so the
three call sites read the same on both platforms and a fourth added later cannot
silently pick the wrong mapping.
"""
import io

DYN = "C:/Users/Admin/Documents/dynarmic/src/dynarmic"


def patch(path, edits, guard):
    s = io.open(path, encoding="utf-8").read()
    if guard in s:
        print("  %s: already patched" % path.rsplit("/", 1)[-1])
        return
    for old, new in edits:
        n = s.count(old)
        assert n == 1, "%s: matched %d for %r" % (path, n, old[:70])
        s = s.replace(old, new, 1)
    io.open(path, "w", encoding="utf-8", newline="").write(s)
    print("  %s: patched" % path.rsplit("/", 1)[-1])


patch(DYN + "/backend/arm64/address_space.h", [
    ('#include <oaknut/code_block.hpp>\n',
     '#include <oaknut/code_block.hpp>\n'
     '#include <oaknut/dual_code_block.hpp>\n'),

    ('namespace Dynarmic::Backend::Arm64 {\n',
     'namespace Dynarmic::Backend::Arm64 {\n'
     '\n'
     '#ifdef __SWITCH__\n'
     '/// Horizon forbids RWX, so code memory is two aliases of one allocation.\n'
     'using JitCodeBlock = oaknut::DualCodeBlock;\n'
     'inline std::uint32_t* CodeWPtr(const JitCodeBlock& m) { return m.wptr(); }\n'
     'inline std::uint32_t* CodeXPtr(const JitCodeBlock& m) { return m.xptr(); }\n'
     '#else\n'
     'using JitCodeBlock = oaknut::CodeBlock;\n'
     'inline std::uint32_t* CodeWPtr(const JitCodeBlock& m) { return m.ptr(); }\n'
     'inline std::uint32_t* CodeXPtr(const JitCodeBlock& m) { return m.ptr(); }\n'
     '#endif\n'),

    # Horizon needs the transition on every emit, so add it to the condition.
    ('#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)\n'
     '        mem.protect();\n',
     '#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__) || defined(__SWITCH__)\n'
     '        mem.protect();\n'),

    ('#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)\n'
     '        mem.unprotect();\n',
     '#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__) || defined(__SWITCH__)\n'
     '        mem.unprotect();\n'),

    ('    oaknut::CodeBlock mem;\n', '    JitCodeBlock mem;\n'),
], guard="JitCodeBlock")

patch(DYN + "/backend/arm64/address_space.cpp", [
    (', code(mem.ptr(), mem.ptr())\n', ', code(CodeWPtr(mem), CodeXPtr(mem))\n'),

    # Reading emitted code back for disassembly: the executable alias is
    # readable and is the address the printed offsets have to match.
    ('    for (u32* ptr = mem.ptr(); ptr < code.xptr<u32*>(); ptr++) {\n',
     '    for (u32* ptr = CodeXPtr(mem); ptr < code.xptr<u32*>(); ptr++) {\n'),

    # Relocation passes: construct over the real bases, then set_xptr resolves
    # the execute address the block info carries back to where it must be
    # written.
    ('    for (auto [ptr_offset, target] : block_info.relocations) {\n'
     '        CodeGenerator c{mem.ptr(), mem.ptr()};\n',
     '    for (auto [ptr_offset, target] : block_info.relocations) {\n'
     '        CodeGenerator c{CodeWPtr(mem), CodeXPtr(mem)};\n'),
], guard="CodeWPtr")

# The exception handler is a no-op on this target (exception_handler_generic),
# but its signature still has to name the type actually held.
BE = "C:/Users/Admin/Documents/dynarmic/src/dynarmic/backend"
for f, old, new in [
    ("exception_handler.h",
     "    void Register(oaknut::CodeBlock& mem, std::size_t mem_size);",
     "#    ifdef __SWITCH__\n"
     "    void Register(oaknut::DualCodeBlock& mem, std::size_t mem_size);\n"
     "#    else\n"
     "    void Register(oaknut::CodeBlock& mem, std::size_t mem_size);\n"
     "#    endif"),
    ("exception_handler_generic.cpp",
     "void ExceptionHandler::Register(oaknut::CodeBlock&, std::size_t) {",
     "#    ifdef __SWITCH__\n"
     "void ExceptionHandler::Register(oaknut::DualCodeBlock&, std::size_t) {\n"
     "#    else\n"
     "void ExceptionHandler::Register(oaknut::CodeBlock&, std::size_t) {\n"
     "#    endif"),
]:
    p = BE + "/" + f
    s = io.open(p, encoding="utf-8").read()
    if "__SWITCH__" in s:
        print("  %s: already patched" % f)
        continue
    assert s.count(old) == 1, f
    s = s.replace(old, new, 1)
    if "dual_code_block" not in s:
        s = s.replace("#include <oaknut/code_block.hpp>",
                      "#include <oaknut/code_block.hpp>\n#include <oaknut/dual_code_block.hpp>", 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    print("  %s: patched" % f)
