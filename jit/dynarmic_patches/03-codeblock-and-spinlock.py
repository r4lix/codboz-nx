"""Remove the last two uses of oaknut::CodeBlock on Horizon.

CodeBlock is the single-address form -- one mapping that is writable and
executable at the same time -- which Horizon refuses outright. DualCodeBlock has
replaced it in AddressSpace, but the header was still being pulled in (it
includes sys/mman.h, which does not exist here) and one more place still built
one.

That place is SpinLockImpl, and it is worse than an include: it is a namespace
scope object, so its constructor allocates a code block during static
initialisation, before main and before anything can report a failure. On this
console whether executable memory can be had at all depends on how the homebrew
was launched, so a throw there would abort the process with no diagnostic, on a
path that has nothing to do with the JIT working.

It also does not need to be generated code. The two functions it emits are a
plain acquire/release spin lock; written in C++ they are the same instructions
GCC would emit, with no code memory and no static-init hazard. The separate
EmitSpinLockLock/EmitSpinLockUnlock used to inline the lock INTO translated code
are untouched -- those still have to be emitted, and they share the layout this
implementation has to agree with: a 32-bit word, 0 free, 1 held.
"""
import io

OAK = "C:/Users/Admin/Documents/dynarmic/externals/oaknut/include/oaknut/code_block.hpp"
s = io.open(OAK, encoding="utf-8").read()
if "__SWITCH__" not in s:
    s = s.replace("#else\n#    include <sys/mman.h>\n#endif\n",
                  "#elif !defined(__SWITCH__)\n#    include <sys/mman.h>\n#endif\n", 1)
    # Horizon has no single-mapping form. Leaving the class declared but
    # unusable would defer the failure to link time with no explanation; hiding
    # it makes any new use a compile error naming this file.
    s = s.replace("namespace oaknut {\n\nclass CodeBlock {",
                  "namespace oaknut {\n\n"
                  "#ifdef __SWITCH__\n"
                  "// Horizon forbids RWX: there is no single mapping that is both\n"
                  "// writable and executable. Use DualCodeBlock instead.\n"
                  "#else\n"
                  "class CodeBlock {", 1)
    s = s.replace("};\n\n}  // namespace oaknut",
                  "};\n#endif\n\n}  // namespace oaknut", 1)
    assert s.count("#ifdef __SWITCH__") == 1
    io.open(OAK, "w", encoding="utf-8", newline="").write(s)
    print("  oaknut/code_block.hpp: guarded")
else:
    print("  oaknut/code_block.hpp: already patched")

AS = "C:/Users/Admin/Documents/dynarmic/src/dynarmic/backend/arm64/address_space.h"
s = io.open(AS, encoding="utf-8").read()
if "#ifndef __SWITCH__\n#include <oaknut/code_block.hpp>" not in s:
    s = s.replace("#include <oaknut/code_block.hpp>\n#include <oaknut/dual_code_block.hpp>\n",
                  "#ifndef __SWITCH__\n#include <oaknut/code_block.hpp>\n#endif\n"
                  "#include <oaknut/dual_code_block.hpp>\n", 1)
    io.open(AS, "w", encoding="utf-8", newline="").write(s)
    print("  address_space.h: include guarded")
else:
    print("  address_space.h: already patched")

SL = "C:/Users/Admin/Documents/dynarmic/src/dynarmic/common/spin_lock_arm64.cpp"
s = io.open(SL, encoding="utf-8").read()
if "__SWITCH__" in s:
    print("  spin_lock_arm64.cpp: already patched")
else:
    s = s.replace("#include <oaknut/code_block.hpp>\n",
                  "#ifndef __SWITCH__\n#include <oaknut/code_block.hpp>\n#endif\n", 1)

    old = s[s.index("namespace {\n\nstruct SpinLockImpl"):s.index("}  // namespace Dynarmic")]
    new = (
        "#ifdef __SWITCH__\n"
        "\n"
        "// Same lock the emitters above inline into translated code -- a 32-bit\n"
        "// word, 0 free and 1 held, acquired on success and released on unlock --\n"
        "// so the two implementations can contend for the same storage.\n"
        "//\n"
        "// WFE/SEVL are deliberately not reproduced. The emitted version can use\n"
        "// them because it knows the STLR that releases the lock is the event that\n"
        "// wakes it; here YIELD is the portable equivalent and this path is only\n"
        "// reached from the exclusive monitor, which this port does not enable.\n"
        "\n"
        "void SpinLock::Lock() {\n"
        "    while (true) {\n"
        "        int expected = 0;\n"
        "        if (__atomic_compare_exchange_n(&storage, &expected, 1, false,\n"
        "                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {\n"
        "            return;\n"
        "        }\n"
        "        while (__atomic_load_n(&storage, __ATOMIC_RELAXED) != 0) {\n"
        "            __asm__ volatile(\"yield\" ::: \"memory\");\n"
        "        }\n"
        "    }\n"
        "}\n"
        "\n"
        "void SpinLock::Unlock() {\n"
        "    __atomic_store_n(&storage, 0, __ATOMIC_RELEASE);\n"
        "}\n"
        "\n"
        "#else\n"
        "\n" + old +
        "#endif\n"
        "\n")
    s = s.replace(old, new, 1)
    io.open(SL, "w", encoding="utf-8", newline="").write(s)
    print("  spin_lock_arm64.cpp: patched")
