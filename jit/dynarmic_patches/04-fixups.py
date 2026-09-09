"""Three things the first pass missed, all only visible once __SWITCH__ was
actually being defined (the platform module supplies it through FLAGS_INIT, and
passing CMAKE_CXX_FLAGS on the command line silently replaced it -- so the first
build was compiling every Horizon branch out).

1. libnx's BIT(n) macro collides with oaknut's BIT instruction -- Bitwise Insert
   if True, an FPSIMD mnemonic emitted as a member function. Pulling switch.h in
   from a header that oaknut later includes turns two declarations into
   "macro BIT passed 3 arguments". Undefining it right after the include is
   enough: nothing in dynarmic or in this port uses libnx's BIT, and the two
   oaknut uses are the entire collision.

2. exception_handler.h forward-declares oaknut::CodeBlock rather than including
   it, so the class it names has to be forward-declared too.

3. LinkBlockLinks has the second `CodeGenerator{mem.ptr(), mem.ptr()}`. It is
   the same pattern as the one in Link() and needs the same treatment -- which
   is exactly the failure mode the JitCodeBlock alias was meant to make loud
   rather than silent, and did.
"""
import io

DCB = "C:/Users/Admin/Documents/dynarmic/externals/oaknut/include/oaknut/dual_code_block.hpp"
s = io.open(DCB, encoding="utf-8").read()
if "#undef BIT" not in s:
    s = s.replace(
        "#if defined(__SWITCH__)\n#    include <switch.h>\n",
        "#if defined(__SWITCH__)\n"
        "#    include <switch.h>\n"
        "// libnx defines BIT(n) as a macro; oaknut has a BIT instruction (Bitwise\n"
        "// Insert if True) declared as a member function. Nothing here wants the\n"
        "// macro, and leaving it defined breaks every oaknut header included after\n"
        "// this one.\n"
        "#    undef BIT\n", 1)
    io.open(DCB, "w", encoding="utf-8", newline="").write(s)
    print("  dual_code_block.hpp: BIT undefined")
else:
    print("  dual_code_block.hpp: already patched")

EH = "C:/Users/Admin/Documents/dynarmic/src/dynarmic/backend/exception_handler.h"
s = io.open(EH, encoding="utf-8").read()
if "class DualCodeBlock;" not in s:
    s = s.replace(
        "namespace oaknut {\nclass CodeBlock;\n}  // namespace oaknut\n",
        "namespace oaknut {\n"
        "#ifdef __SWITCH__\n"
        "class DualCodeBlock;\n"
        "#else\n"
        "class CodeBlock;\n"
        "#endif\n"
        "}  // namespace oaknut\n", 1)
    assert "class DualCodeBlock;" in s
    io.open(EH, "w", encoding="utf-8", newline="").write(s)
    print("  exception_handler.h: DualCodeBlock forward-declared")
else:
    print("  exception_handler.h: already patched")

AS = "C:/Users/Admin/Documents/dynarmic/src/dynarmic/backend/arm64/address_space.cpp"
s = io.open(AS, encoding="utf-8").read()
n = s.count("        CodeGenerator c{mem.ptr(), mem.ptr()};\n")
if n:
    s = s.replace("        CodeGenerator c{mem.ptr(), mem.ptr()};\n",
                  "        CodeGenerator c{CodeWPtr(mem), CodeXPtr(mem)};\n")
    io.open(AS, "w", encoding="utf-8", newline="").write(s)
    print("  address_space.cpp: %d more CodeGenerator site(s) fixed" % n)
else:
    print("  address_space.cpp: already patched")

# Nothing may still reach for the single-mapping accessor.
assert "mem.ptr()" not in io.open(AS, encoding="utf-8").read(), "a mem.ptr() remains"
print("  address_space.cpp: no mem.ptr() left")
