"""Horizon (Nintendo Switch) branch for oaknut's DualCodeBlock.

Horizon has no mmap and forbids RWX outright, so the POSIX branch -- memfd_create
plus two mmaps of one fd -- has no equivalent. What it does have is exactly the
same shape one level up: libnx's Jit object takes a chunk of heap and exposes it
twice, once writable and once executable, which is what wptr()/xptr() mean.

Using libnx's Jit rather than svcCreateCodeMemory directly is deliberate. There
are two mechanisms on this console -- CodeMemory syscalls (4.0.0+, available when
the process was launched with syscall hints) and svcSetProcessMemoryPermission --
and which one is usable depends on how the homebrew was launched, not on
anything we control. jitCreate picks; the difference leaks out only in whether
the two aliases are live simultaneously or have to be toggled, and calling
transition_to_writable/executable around emission is correct under both.

That toggle is free in the CodeMemory case and a syscall pair in the other, and
it lands on dynarmic's existing UnprotectCodeMemory/ProtectCodeMemory calls,
which already bracket exactly the emission windows -- so the cost is paid per
block compiled, never per block executed.
"""
import io, sys

P = r"C:\Users\Admin\Documents\dynarmic\externals\oaknut\include\oaknut\dual_code_block.hpp"
s = io.open(P, encoding="utf-8").read()

if "__SWITCH__" in s:
    print("oaknut: already patched")
    sys.exit(0)

# ---- includes -------------------------------------------------------------
s = s.replace(
    "#if defined(_WIN32)\n"
    "#    define NOMINMAX\n"
    "#    include <windows.h>\n",

    "#if defined(__SWITCH__)\n"
    "#    include <switch.h>\n"
    "#elif defined(_WIN32)\n"
    "#    define NOMINMAX\n"
    "#    include <windows.h>\n", 1)

# ---- constructor ----------------------------------------------------------
s = s.replace(
    "#if defined(_WIN32)\n"
    "        m_wmem = m_xmem = (std::uint32_t*)VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);\n"
    "        if (m_wmem == nullptr)\n"
    "            throw std::bad_alloc{};\n",

    "#if defined(__SWITCH__)\n"
    "        // jitCreate wants a page multiple; round up rather than fail, and keep\n"
    "        // m_size in step so invalidate_all() does not walk off the end.\n"
    "        m_size = size = (size + 0xFFF) & ~std::size_t(0xFFF);\n"
    "        if (R_FAILED(jitCreate(&m_jit, size)))\n"
    "            throw std::bad_alloc{};\n"
    "        m_wmem = (std::uint32_t*)jitGetRwAddr(&m_jit);\n"
    "        m_xmem = (std::uint32_t*)jitGetRxAddr(&m_jit);\n"
    "        if (m_wmem == nullptr || m_xmem == nullptr) {\n"
    "            jitClose(&m_jit);\n"
    "            throw std::bad_alloc{};\n"
    "        }\n"
    "        // Leave the block writable: the caller emits before it ever executes,\n"
    "        // and dynarmic's UnprotectCodeMemory/ProtectCodeMemory pair drives the\n"
    "        // transitions from here on.\n"
    "        jitTransitionToWritable(&m_jit);\n"
    "#elif defined(_WIN32)\n"
    "        m_wmem = m_xmem = (std::uint32_t*)VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);\n"
    "        if (m_wmem == nullptr)\n"
    "            throw std::bad_alloc{};\n", 1)

# ---- destructor -----------------------------------------------------------
s = s.replace(
    "    ~DualCodeBlock()\n"
    "    {\n"
    "#if defined(_WIN32)\n",

    "    ~DualCodeBlock()\n"
    "    {\n"
    "#if defined(__SWITCH__)\n"
    "        jitClose(&m_jit);\n"
    "#elif defined(_WIN32)\n", 1)

# ---- protect / unprotect --------------------------------------------------
# Upstream DualCodeBlock has neither, because on every platform it supports both
# aliases are permanently live. Horizon may need the toggle, so add them and let
# dynarmic call them unconditionally.
s = s.replace(
    "    /// Invalidate should be used with executable memory pointers.\n",

    "    /// W^X transitions. No-ops except on Horizon, where whether they do\n"
    "    /// anything depends on which JIT mechanism jitCreate selected.\n"
    "    void protect()\n"
    "    {\n"
    "#if defined(__SWITCH__)\n"
    "        jitTransitionToExecutable(&m_jit);\n"
    "#endif\n"
    "    }\n"
    "\n"
    "    void unprotect()\n"
    "    {\n"
    "#if defined(__SWITCH__)\n"
    "        jitTransitionToWritable(&m_jit);\n"
    "#endif\n"
    "    }\n"
    "\n"
    "    /// Invalidate should be used with executable memory pointers.\n", 1)

# ---- members --------------------------------------------------------------
s = s.replace(
    "protected:\n"
    "#if !defined(_WIN32) && !defined(__APPLE__)\n"
    "    int fd = -1;\n"
    "#endif\n",

    "protected:\n"
    "#if defined(__SWITCH__)\n"
    "    Jit m_jit{};\n"
    "#elif !defined(_WIN32) && !defined(__APPLE__)\n"
    "    int fd = -1;\n"
    "#endif\n", 1)

# The POSIX include block is still selected on Horizon by the trailing #else;
# it pulls sys/mman.h, which does not exist. Guard it.
s = s.replace(
    "#else\n"
    "#    if !defined(_GNU_SOURCE)\n"
    "#        define _GNU_SOURCE\n"
    "#    endif\n"
    "#    include <sys/mman.h>\n",

    "#elif !defined(__SWITCH__)\n"
    "#    if !defined(_GNU_SOURCE)\n"
    "#        define _GNU_SOURCE\n"
    "#    endif\n"
    "#    include <sys/mman.h>\n", 1)

assert "__SWITCH__" in s
io.open(P, "w", encoding="utf-8", newline="").write(s)
print("oaknut: DualCodeBlock has a Horizon branch")
