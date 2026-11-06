"""
x86 → C instruction lifter.

Translates individual x86 instructions (and common multi-instruction
patterns like cmp+jcc) into C statements using the recomp_types.h macros.

Register model:
  - eax, ebx, ecx, edx, esi, edi, ebp: uint32_t locals
  - esp: uint32_t local (stack pointer)
  - FPU: shared double g_fp_stack[8] with g_fp_top index

Memory model:
  - MEM8/MEM16/MEM32 macros for memory access at flat addresses
  - Xbox data sections mapped at original VAs
"""

import os
import re
import struct

from .disasm import Instruction, Operand, LOOP_JUMPS
from .config import is_code_address, is_data_address, va_to_file_offset
import translation_moves as _project_moves
import translation_integer as _project_integer
import translation_bits as _project_bits
import translation_product as _project_product
import translation_bitops as _project_bitops
import translation_exchange as _project_exchange
import translation_compare as _project_compare
import translation_strings as _project_strings
import translation_conditions as _project_conditions
import translation_sse as _project_sse
import translation_mmx as _project_mmx
import translation_x87 as _project_x87
import translation_control as _project_control

# Function export names of the Windows libraries the host exe links
# (kernel32/user32/gdi32/advapi32/winmm/ws2_32/ole32/dbghelp/...). A guest
# function that shares one of these names collides with the import at link
# time (LNK2005) -- every Xbox-era game re-exports shims named like Win32
# APIs (CreateThread, GetLastError, QueryPerformanceCounter, SetEvent, ...).
# Generated from the x64 import libs of the Windows SDK with:
#   Get-ChildItem "$env:WINSDK/Lib/*/um/x64" -Filter *.lib | ForEach-Object {
#     & dumpbin /exports $_ }  # then keep the indent-only identifier lines
_WIN32_EXPORTS = set()
_DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
_WIN32_EXPORTS_FILE = os.path.join(_DATA_DIR, "win32_api_names.txt")
if os.path.exists(_WIN32_EXPORTS_FILE):
    with open(_WIN32_EXPORTS_FILE, encoding="ascii") as _f:
        _WIN32_EXPORTS.update(line.strip() for line in _f if line.strip())


# C identifiers a recompiled function name must not be: the generated TUs
# include the C standard headers (math.h/string.h through recomp_types.h,
# stdlib.h in the dispatch TU), and a recompiled image carries its own copies
# of the CRT helpers -- Black has a function literally named `onexit`.
# Emitting `void onexit(void);` next to UCRT's `onexit_t __cdecl onexit(
# onexit_t)` is a redefinition with different type modifiers and cl fails with
# C2373. The host Win32 export names are folded in too: the host exe links
# kernel32.lib et al., and Xbox games re-export shims named exactly like the
# APIs they wrap, so the same clash happens there for the linker. Mangle with
# the address, the same suffixed-<addr> scheme func_id already uses for
# duplicate names.
#
# This is the last guard before a name becomes a C token, and the only one
# every name source passes through -- Ghidra, IDA, a hand-written symbols
# file. tools/ghidra_naming/merge_names.py filters a similar set earlier, but
# it only sees the Ghidra path, so this list must not be the smaller of the
# two. Over-mangling costs an address suffix on a name; under-mangling costs
# a build.
_FUNC_RESERVED_IDENT = frozenset({
    # C and C++ keywords
    "asm", "auto", "break", "case", "char", "const", "continue",
    "default", "do", "double", "else", "enum", "extern", "float",
    "for", "goto", "if", "inline", "int", "long", "register",
    "restrict", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "typedef", "union", "unsigned", "void",
    "volatile", "while",
    # <string.h>
    "memchr", "memcmp", "memcpy", "memmove", "memset", "strcat",
    "strchr", "strcmp", "strcoll", "strcpy", "strcspn", "strerror",
    "strlen", "strncat", "strncmp", "strncpy", "strpbrk", "strrchr",
    "strspn", "strstr", "strtok", "strxfrm", "strdup", "strnlen",
    # <math.h> C89
    "acos", "asin", "atan", "atan2", "ceil", "cos", "cosh", "exp",
    "fabs", "floor", "fmod", "frexp", "ldexp", "log", "log10", "modf",
    "pow", "sin", "sinh", "sqrt", "tan", "tanh",
    # <math.h> C99 classification and comparison. These are function-like
    # *macros*, which makes them the worst members of this set: a function
    # named `isnan` does not collide, it expands. `void isnan(void);` becomes
    # `void (fpclassify(void) == FP_NAN);` and the compiler reports a bad
    # parameter declarator inside corecrt_math.h, naming neither the guest
    # function nor the clash. Reported from a FIFA Street 2 port.
    "fpclassify", "isfinite", "isgreater", "isgreaterequal", "isinf",
    "isless", "islessequal", "islessgreater", "isnan", "isnormal",
    "isunordered", "signbit",
    # <math.h> C99 functions
    "acosh", "asinh", "atanh", "cbrt", "copysign", "erf", "erfc", "exp2",
    "expm1", "fdim", "fma", "fmax", "fmin", "hypot", "ilogb", "lgamma",
    "llrint", "llround", "log1p", "log2", "logb", "lrint", "lround",
    "nan", "nearbyint", "nextafter", "nexttoward", "remainder", "remquo",
    "rint", "round", "scalbln", "scalbn", "tgamma", "trunc",
    # <stdlib.h>
    "abort", "abs", "atexit", "atof", "atoi", "atol", "bsearch",
    "calloc", "div", "exit", "free", "getenv", "labs", "ldiv", "malloc",
    "mblen", "mbstowcs", "mbtowc", "onexit", "qsort", "rand", "realloc",
    "srand", "strtod", "strtol", "strtoul", "system", "wctomb",
    "wcstombs",
    # <stdlib.h> C99
    "atoll", "llabs", "lldiv", "strtof", "strtold", "strtoll", "strtoull",
    # <setjmp.h>
    "longjmp", "setjmp",
    # Host Win32 API export names (data/win32_api_names.txt) so a guest
    # function named like a linked import does not collide at link time.
}) | _WIN32_EXPORTS


def _func_ident(addr, name):
    """C identifier for the recompiled function at addr.

    Every place a func_db name becomes a C token -- the function definition,
    its forward declaration, call sites and the dispatch table -- must agree,
    or the link fails. Reserved names get the same ``_<addr>`` suffix func_id
    already gives duplicate names.
    """
    base = name if name else f"sub_{addr:08X}"
    if base in _FUNC_RESERVED_IDENT:
        return f"{base}_{addr:08X}"
    return base


# Operand expressions belong to the project; the instruction handlers remain here.
from translation_operands import (
    _SYSTEM_REGS, _fmt_reg, _fmt_set_reg, _fmt_imm, _mem_accessor,
    _smem_accessor, _fmt_fpu_operand, _fmt_mem, _fmt_mem_read,
    _fmt_mem_write, _operand_width, _fmt_operand_read, _fmt_operand_write,
)


# ── Condition code mapping ───────────────────────────────────

# Condition descriptions and evaluation are project-owned.
COND_MAP = _project_conditions.COND_MAP

# Instructions that set arithmetic flags (primary set, fully handled)
FLAG_SETTERS = frozenset({
    "cmp", "test", "sub", "add", "and", "or", "xor",
    "inc", "dec", "neg", "shl", "shr", "sar", "imul", "mul", "adc", "sbb",
    "comiss", "comisd", "ucomiss", "ucomisd",  # SSE float compare
})

# Arithmetic whose carry-out the lifter computes into _cf next to the write.
#
# A jb/jae reading CF after one of these is exact, which matters because the
# generic fallback is a _flags variable nothing ever assigns -- the condition
# came out always-false. MSVC's bit-oriented decoders are built entirely from
# this shape: "add reg, reg" to shift the top bit into CF, then jae on it.
# Setters that WRITE their destination and whose ZF/SF a later jcc reads. The
# destination can be overwritten between the two, so each of these publishes
# its result into _fa/_fas next to the write and the condition reads that --
# the rule inc/dec has followed since "Result at the flag-setting
# instruction, before later MOVs". See Lifter._result_snapshot.
_RESULT_SNAPSHOT_SETTERS = frozenset({
    "and", "or", "xor", "adc", "sbb", "neg",
    "shl", "sal", "shr", "sar", "shld", "shrd",
    "add", "sub",
})
# ...and the two whose conditions also need the SOURCE, to recover the
# original destination from the result. Theirs goes into _fb, captured before
# the write because `sub eax, eax` would otherwise snapshot an operand it has
# already destroyed.
_RESULT_SRC_SETTERS = frozenset({"add", "sub"})

CF_TRACKED = frozenset({
    "add", "sub", "adc", "sbb", "shl", "shr", "sar", "shld", "shrd",
})

# bt reports CF without touching the operand, so its CF can be rebuilt at the
# consumer by reading the bit again. The other three WRITE the bit they report,
# so the same read gives the value after the write -- always 1 after bts,
# always 0 after btr, inverted after btc. Their CF only exists if it was
# captured before the write, which is what _cf is for, so they are listed
# separately and _function_needs_cf treats them as CF producers.
BT_MODIFY = frozenset({"bts", "btr", "btc"})

# Arithmetic that writes its destination and leaves ZF as (destination == 0).
# A join can unify two different setters from this set when they share a
# destination register, because a je or jne then means the same thing on both
# edges. cmp and test are deliberately absent: they write no destination.
ZF_FROM_DEST = frozenset({
    "sub", "add", "and", "or", "xor", "inc", "dec", "neg",
    "adc", "sbb", "shl", "shr", "sar",
})

# Additional instructions that modify EFLAGS (tracked but handled as generic)
_EFLAGS_SETTERS = frozenset({
    "shld", "shrd", "rol", "ror", "rcl", "rcr",  # Shifts/rotates set CF
    "bsf", "bsr",       # Bit scan sets ZF
    "bt", "bts", "btr", "btc",  # Bit test sets CF
    "cmpxchg",           # Compare-and-exchange sets ZF
    "xadd",              # Exchange-and-add sets flags
})

# Instructions with undefined/unpredictable flags (clear tracking)
_FLAGS_UNDEFINED = frozenset({
    "div", "idiv",  # Division flags are undefined
    "rdtsc", "cpuid",      # Special instructions
    # "lock xadd" was here, described as complex flag behaviour. It is not:
    # LOCK only makes the read-modify-write atomic and leaves xadd's flags
    # exactly as the unlocked form sets them, from the sum. Listing it here
    # cleared flag tracking, so the jcc that follows fell back to the _flags
    # placeholder, which is always 0. In Jet Set Radio Future that turned
    # every COM-style Release() -- lock xadd on the refcount, jne to return
    # while it is still non-zero -- into an unconditional destroy, and the
    # second Release() on an object double-freed it. The prefix is stripped
    # where flags are tracked instead; see the LOCK note there.
    # popfd REPLACES every flag with whatever was pushed. Its flags are not
    # architecturally undefined -- they are simply not knowable from the
    # instruction stream -- but the tracking action is the same: whatever the
    # last comparison left is gone, and a jcc after it must not be resolved
    # from that comparison. It used to sit in _EFLAGS_PRESERVE, next to
    # pushfd, which does read-and-preserve and does belong there.
    "popfd",
})

# Instructions that do NOT modify EFLAGS (preserve flag tracking)
_EFLAGS_PRESERVE = frozenset({
    # General-purpose data movement / stack
    "mov", "lea", "push", "pop", "nop", "leave", "ret",
    "movzx", "movsx", "xchg", "bswap",
    "cdq", "cwde", "cbw", "cwd",
    "lahf",
    "not",  # NOT does not modify flags
    "call",
    "int3", "int", "wait",
    "cld", "std", "cli", "sti",
    # LOOP counts ECX down and branches on it; LOOPE/LOOPNE additionally
    # read ZF. None of the three write EFLAGS, so a comparison before the
    # loop still answers the jcc after it.
    "loop", "loope", "loopne",
    # pushfd READS the flags and leaves them alone, so it belongs here.
    # popfd does NOT -- see _FLAGS_UNDEFINED.
    "pushfd", "pushal",
    "sgdt", "ljmp", "sfence",
    # SSE scalar float
    "movss", "movsd",
    "addss", "subss", "mulss", "divss",
    "minss", "maxss", "sqrtss", "rsqrtss", "rcpss",
    "addsd", "subsd", "mulsd", "divsd",
    "minsd", "maxsd", "sqrtsd",
    "cvtsi2ss", "cvtss2si", "cvttss2si",
    "cvtsi2sd", "cvtsd2si", "cvttsd2si",
    "cvtss2sd", "cvtsd2ss",
    "cmpss", "cmpsd",
    "cmpltss", "cmpeqss", "cmpleps", "cmpneqss",
    # SSE packed float
    "movaps", "movups", "movlps", "movhps", "movlhps", "movhlps",
    "addps", "subps", "mulps", "divps",
    "minps", "maxps", "sqrtps", "rsqrtps", "rcpps",
    "shufps", "unpcklps", "unpckhps",
    "andps", "orps", "xorps", "andnps",
    "cmpps", "cmpneqps",
    "movmskps",
    # SSE2 packed double
    "movapd", "movupd",
    "addpd", "subpd", "mulpd", "divpd",
    # SSE/MMX integer
    "movd", "movq", "movntq",
    "cvtps2pi", "cvttps2pi", "pinsrw", "pextrw",
    "emms",
    "paddb", "paddw", "paddd", "paddq",
    "psubb", "psubw", "psubd",
    "paddsb", "paddsw", "paddusb", "psubsb", "psubsw", "psubusb",
    "pavgb", "pavgw", "pminsw", "pmaxsw", "psadbw",
    "pmullw", "pmulhw", "pmulhuw", "pmaddwd",
    "pand", "pandn", "por", "pxor",
    "pcmpeqb", "pcmpeqw", "pcmpeqd",
    "pcmpgtb", "pcmpgtw", "pcmpgtd",
    "psllw", "pslld", "psllq",
    "psrlw", "psrld", "psrlq",
    "psraw", "psrad",
    "pshufw", "pshufd", "pshufhw", "pshuflw",
    "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
    "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
    "packsswb", "packssdw", "packuswb",
    "pmovmskb",
    "paddusw", "psubusw",
    # String operations (without rep prefix)
    "stosb", "stosw", "stosd",
    "movsb", "movsw", "movsd",
    "lodsb", "lodsw", "lodsd",
    # Prefetch hints
    "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
})


def _has_xmm_operand(ops):
    """True for the SSE reading of a mnemonic the string operations share.

    Only "movsd" is ambiguous in the dispatcher today -- "cmpsd" is the other
    mnemonic x86 overloads this way, but it has no SSE lifter here yet. The
    test is written over the operands rather than the mnemonic so it stays
    correct if one is added.
    """
    return any(op is not None and getattr(op, "type", None) == "reg"
               and (getattr(op, "reg", None) or "").startswith("xmm")
               for op in (ops or ()))


zf_expression = _project_conditions.zf_expression
_make_condition = _project_conditions.make_condition
_make_setcc_value = _project_conditions.setcc
_make_lahf_value = _project_conditions.lahf
_make_cmovcc_cond = _project_conditions.cmovcc


_emit_cond_goto = _project_control.conditional


def try_match_cmp_jcc(insns, idx, lifter=None):
    """
    Try to match a cmp/test + jcc pattern starting at insns[idx].
    Returns (c_statement, num_consumed) or None.
    """
    if idx + 1 >= len(insns):
        return None

    first = insns[idx]
    second = insns[idx + 1]

    if first.mnemonic not in ("cmp", "test") or not second.is_cond_jump:
        return None

    if len(first.operands) < 2:
        return None

    # Same normalised form the snapshot and the recorded state use.
    result = _make_condition(second.mnemonic,
                             *normalise_zero_test(first.mnemonic, first.operands))
    if not result:
        return None

    cond_expr, desc = result
    target = second.jump_target
    stmt = _emit_cond_goto(cond_expr, second.mnemonic, desc, target, lifter)
    return (stmt, 2)


# ── Single instruction lifting ───────────────────────────────

# MSVC's __SEH_prolog establishes the caller's frame pointer, so the lifter has
# to know which function it is. The address is per-title, and hardcoding it
# meant every other game silently got no frame set up after the call: ebp kept
# whatever stale value it had, and the first ebp-relative local access read
# through it. In Halo that surfaced as a read of 0xFFFFFFFC (ebp=0, [ebp-4]).
#
# Both helpers are compiler boilerplate with distinctive bodies, so detect them
# rather than asking every project to look them up by hand.
#
#   __SEH_prolog   mov eax, fs:[0]        64 A1 00 00 00 00
#                  lea ebp, [esp+0x10]    8D 6C 24 10
#   __SEH_epilog   mov fs:[0], ecx        64 89 0D 00 00 00 00
#                  leave; push ecx; ret   C9 51 C3
#
# MSVC ships two of these, not one, and they differ only in how far up the
# stack the frame lands. __SEH_prolog pushes four dwords and takes ebp to
# esp+0x10; __EH_prolog -- the C++ exception-handling flavour, emitted for a
# function with a try block -- pushes three and takes it to esp+0x0C. Matching
# only the first meant a title built with C++ EH had no prologue detected at
# all, so nothing ever read the frame back and every caller of it ran on an
# inherited frame pointer belonging to some outer function. It does not crash
# there: the caller writes [ebp-40] and reads [ebp-40] and looks fine, while
# actually scribbling on another function's locals. Outrun 2 surfaced it as a
# std::map pointer that became null with nothing having written a null --
# docs/technical/eh-prolog-frames.md has the whole trail.
_SEH_PROLOG_FS_READ = b"\x64\xa1\x00\x00\x00\x00"
_SEH_PROLOG_LEAS = (b"\x8d\x6c\x24\x10",   # __SEH_prolog
                    b"\x8d\x6c\x24\x0c")   # __EH_prolog
_SEH_EPILOG_MARKERS = (b"\x64\x89\x0d\x00\x00\x00\x00", b"\xc9\x51\xc3")

# Both are tiny; a large match is something else that happens to touch fs:[0].
_SEH_PROLOG_MAX_SIZE = 128
_SEH_EPILOG_MAX_SIZE = 64


# MSVC compiles `throw x` into a call to _CxxThrowException, which never
# returns. This runtime cannot unwind, so the call falls through to the int3
# the compiler puts after it and the throw simply returns, on a stack nothing
# cleaned up. Naming the function lets the runtime report the throw at the
# point it happens, with the type that was thrown, instead of leaving the
# consequences to surface somewhere unrelated. See
# docs/technical/cpp-exceptions.md.
#
# The function is found through the static EHExceptionRecord template it
# copies onto its own stack. That template is unmistakable:
#
#   +0x00  0xE06D7363   the C++ exception code, 'msc' | 0xE0000000
#   +0x04  1            EXCEPTION_NONCONTINUABLE
#   +0x10  3            NumberParameters
#   +0x14  0x19930520   EH_MAGIC_NUMBER1
#
# so find the template first, then the small function that references its
# address. Matching on 0x19930520 alone would not do: it appears 146 times in
# Outrun 2, once in every function's exception state table, while the record
# code appears eight.
_EH_EXCEPTION_CODE = 0xE06D7363
_EH_MAGIC_NUMBER1 = 0x19930520
_CXX_THROW_MAX_SIZE = 256


def _is_eh_record_template(xbe_data, va):
    """Whether guest VA `va` holds MSVC's EHExceptionRecord template."""
    from .config import va_to_file_offset
    import struct

    off = va_to_file_offset(va)
    if off is None or off + 32 > len(xbe_data):
        return False
    w = struct.unpack("<8I", xbe_data[off:off + 32])
    return (w[0] == _EH_EXCEPTION_CODE and w[4] == 3
            and w[5] == _EH_MAGIC_NUMBER1)


def detect_cxx_throw(func_db, xbe_data, verbose=False):
    """Locate _CxxThrowException, or None for a title that never throws."""
    import struct

    if not func_db or not xbe_data:
        return None

    from .config import va_to_file_offset

    for addr in sorted(func_db):
        info = func_db[addr]
        try:
            size = int(info.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if size <= 0 or size > _CXX_THROW_MAX_SIZE:
            continue
        off = va_to_file_offset(addr)
        if off is None or off + size > len(xbe_data):
            continue
        body = xbe_data[off:off + size]

        # Any 4-byte window could be the template's address as an immediate.
        for k in range(0, len(body) - 3):
            cand = struct.unpack("<I", body[k:k + 4])[0]
            if cand and _is_eh_record_template(xbe_data, cand):
                if verbose:
                    import sys
                    print(f"  _CxxThrowException: 0x{addr:08X} "
                          f"(record template at 0x{cand:08X})", file=sys.stderr)
                return addr

    if verbose:
        import sys
        print("  _CxxThrowException: not found "
              "(this title may not use C++ exceptions)", file=sys.stderr)
    return None


def detect_seh_helpers(func_db, xbe_data, verbose=False):
    """Locate __SEH_prolog / __SEH_epilog in the target binary.

    Returns (prologs, epilogs) as tuples of addresses, either possibly empty,
    which is normal for a title whose CRT does not use them.

    A title can ship more than one of each. MSVC emits __SEH_prolog for a
    function that only needs structured exception handling and __EH_prolog for
    one with C++ exception handling, and anything built from mixed sources
    links both. Outrun 2 has __SEH_prolog at 0x00185434 and __EH_prolog at
    0x00185AD0. Returning only the first meant the second was treated as an
    ordinary call, so none of its callers ever read their frame pointer back.
    """
    from .config import va_to_file_offset

    prologs, epilogs = [], []

    def _size_of(info):
        # "end" is a hex string in functions.json but BatchTranslator rewrites
        # it to an int in place, so accept either.
        try:
            size = int(info.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if size:
            return size
        end = info.get("end")
        if isinstance(end, str):
            try:
                end = int(end, 16)
            except ValueError:
                return 0
        return (end - addr) if isinstance(end, int) else 0

    for addr in sorted(func_db):
        info = func_db[addr]
        size = _size_of(info)
        if size <= 0 or size > _SEH_PROLOG_MAX_SIZE:
            continue

        offset = va_to_file_offset(addr)
        if offset is None or xbe_data is None or offset + size > len(xbe_data):
            continue
        body = xbe_data[offset:offset + size]

        if (_SEH_PROLOG_FS_READ in body
                and any(lea in body for lea in _SEH_PROLOG_LEAS)):
            prologs.append(addr)
        elif (size <= _SEH_EPILOG_MAX_SIZE
                and all(m in body for m in _SEH_EPILOG_MARKERS)):
            epilogs.append(addr)

    if verbose:
        import sys
        fmt = lambda xs: (", ".join(f"0x{a:08X}" for a in xs)
                          if xs else "not found")
        print(f"  SEH helpers: prologs {fmt(prologs)}, "
              f"epilogs {fmt(epilogs)}", file=sys.stderr)

    return tuple(prologs), tuple(epilogs)


# MSVC's setjmp/longjmp pair, found by the "VC20" cookie the CRT stamps into
# every jmp_buf. setjmp stores it, longjmp compares against it, so the two are
# told apart by the opcode carrying the constant rather than by the constant
# itself -- which appears in both.
#
#   setjmp    mov dword ptr [edx+20h], 56433230h    C7 42 20 30 32 43 56
#   longjmp   cmp eax, 56433230h                    3D 30 32 43 56
#
# Both markers are unique across a whole title's image, so a hit is the
# function and no size bound is needed.
_SETJMP_MARKER  = bytes.fromhex("c7422030324356")
_LONGJMP_MARKER = bytes.fromhex("3d30324356")


def detect_setjmp_helpers(func_db, xbe_data, verbose=False):
    """Locate the CRT's setjmp and longjmp in the target binary.

    Returns (setjmp_addr, longjmp_addr); either may be None. A title whose CRT
    has neither is normal -- nothing in it uses non-local jumps.
    """
    from .config import va_to_file_offset

    if not xbe_data:
        return None, None

    found = {}
    for marker, key in ((_SETJMP_MARKER, "setjmp"),
                        (_LONGJMP_MARKER, "longjmp")):
        for addr in sorted(func_db):
            info = func_db[addr]
            end = info.get("end")
            if isinstance(end, str):
                try:
                    end = int(end, 16)
                except ValueError:
                    continue
            size = (end - addr) if isinstance(end, int) else 0
            if size <= 0 or size > 512:
                continue
            offset = va_to_file_offset(addr)
            if offset is None or offset + size > len(xbe_data):
                continue
            if marker in xbe_data[offset:offset + size]:
                found[key] = addr
                break

    if verbose:
        import sys
        fmt = lambda a: f"0x{a:08X}" if a else "not found"
        print(f"  CRT non-local jumps: setjmp {fmt(found.get('setjmp'))}, "
              f"longjmp {fmt(found.get('longjmp'))}", file=sys.stderr)

    return found.get("setjmp"), found.get("longjmp")


normalise_zero_test = _project_compare.normalise_zero_test


class Lifter:
    """Translates x86 instructions to C statements."""

    def __init__(self, func_db=None, label_db=None, abi_db=None, xbe_data=None,
                 seh_prolog=None, seh_epilog=None,
                 setjmp_fn=None, longjmp_fn=None, manual_functions=None):
        """
        func_db: dict of func_addr → func_info (for naming call targets)
        label_db: dict of addr → name (for kernel imports, etc.)
        abi_db: dict of addr → ABI info (for calling conventions)
        xbe_data: raw XBE file bytes (for reading jump tables)
        seh_prolog/seh_epilog: override the detected __SEH_prolog/__SEH_epilog
        manual_functions: addresses replaced through recomp_lookup_manual
        """
        self.func_db = func_db or {}
        self.label_db = label_db or {}
        self.abi_db = abi_db or {}
        self.xbe_data = xbe_data
        self.manual_functions = set(manual_functions or ())
        self._fp_top = 0  # FPU stack top index
        self.func_start = 0  # Set per-function by translator
        self.func_end = 0
        self.needs_cf = False  # Set per-function by translator (has adc/sbb)
        self.publishes_ebp = False  # Set per-function: has a real frame
        self.trace_exit_name = None  # Set per-function when traced
        self.force_return_value = None   # Set per-function by --force-return
        # Every direct call target we emit a name for, as {addr: name}. The
        # batch translator diffs this against the functions it actually defined
        # so it can stub out the remainder (see translate_batch_split).
        self.referenced_calls = {}
        # Mnemonics that fell through to the TODO comment, as
        # {mnemonic: [addr, ...]}. An unimplemented instruction becomes a
        # comment, which is a silent no-op in the generated C: Wreckless spent
        # a whole bring-up dying inside RtlAllocateHeap because `bsf eax, ecx`
        # was a comment, so the heap's free-list bitmap scan returned its own
        # input and the allocator handed out the address of an empty list head.
        # The translator reports this at the end of a run, so the next one
        # costs a line of output instead of an afternoon.
        self.unimplemented = {}
        # callee address -> the argument bytes its own `ret N` pops, or None.
        self._pop_cache = {}

        # Detect if either is missing, so overriding one does not silently
        # leave the other unset -- that is the bug this whole path fixes.
        found_prologs = found_epilogs = ()
        if (seh_prolog is None or seh_epilog is None) and self.func_db:
            found_prologs, found_epilogs = detect_seh_helpers(
                self.func_db, xbe_data)

        def _as_set(override, found):
            """An explicit address, a list of them, or whatever was detected."""
            if override is None:
                return frozenset(found)
            if isinstance(override, int):
                return frozenset((override,))
            return frozenset(override)

        self.SEH_PROLOGS = _as_set(seh_prolog, found_prologs)
        self.SEH_EPILOGS = _as_set(seh_epilog, found_epilogs)

        self.CXX_THROW = (detect_cxx_throw(self.func_db, xbe_data)
                          if self.func_db else None)
        self.SETJMP_FN = setjmp_fn
        self.LONGJMP_FN = longjmp_fn
        self.jump_table_targets = {}
        # call-site VA -> targets the title was measured reaching from it
        # (tools/recomp/output/icall_sites.json). A site whose whole recorded
        # set is small and translated is guarded with direct calls; the
        # generic dispatch stays as the fallback, so an unseen target is
        # slow, never wrong. See _lift_call.
        self.icall_site_targets = {}
        # Addresses loaded as immediates into a register inside the current
        # function, used to resolve `jmp <reg>`. See _lift_jmp.
        self.imm_code_refs = set()

    def _call_target_name(self, addr):
        """Get the name for a call target address.

        func_db wins over label_db. The function definition is emitted from
        func_db, so consulting labels first meant a renamed function was
        *defined* as cseries__sub_0008DB80 but *called* as sub_0008DB80 -- the
        disassembler's generic auto-label -- and the link failed on every
        function any naming pass had touched. Labels still cover call targets
        that are not known function starts.
        """
        if addr in self.func_db:
            # call_name: a function recomp_manual.c wraps is emitted as
            # sub_X_gen but called as sub_X, the wrapper.
            info = self.func_db[addr]
            name = info.get("call_name") or info.get("name", f"sub_{addr:08X}")
        elif addr in self.label_db:
            name = self.label_db[addr]
        else:
            name = f"sub_{addr:08X}"
        name = _func_ident(addr, name)
        self.referenced_calls[addr] = name
        return name

    def lift_instruction(self, insn):
        """
        Translate a single x86 instruction to one or more C statements.
        Returns a list of C statement strings.
        """
        m = insn.mnemonic
        ops = insn.operands
        nops = len(ops)

        # ── NOP ──
        if m == "nop" or (m == "lea" and nops == 2 and
                          ops[0].type == "reg" and ops[1].type == "mem" and
                          ops[1].mem_base == ops[0].reg and
                          not ops[1].mem_index and ops[1].mem_disp == 0):
            return [f"/* nop */"]

        # ── Data movement ──
        if m == "mov":
            return self._lift_mov(insn, ops)
        if m == "movzx":
            return self._lift_movzx(insn, ops)
        if m == "movsx":
            return self._lift_movsx(insn, ops)
        if m == "lea":
            return self._lift_lea(insn, ops)
        if m in ("xchg", "lock xchg"):
            return self._lift_xchg(insn, ops)

        # ── Stack ──
        if m == "push":
            return self._lift_push(insn, ops)
        if m == "pop":
            return self._lift_pop(insn, ops)

        # ── Arithmetic ──
        if m in ("add", "sub", "and", "or", "xor"):
            return self._lift_alu_binop(insn, ops, m)
        if m in ("inc", "dec"):
            return self._lift_inc_dec(insn, ops, m)
        if m == "neg":
            return self._lift_neg(insn, ops)
        if m == "not":
            return self._lift_not(insn, ops)
        if m == "imul":
            return self._lift_imul(insn, ops)
        if m in ("mul", "div", "idiv"):
            return self._lift_muldiv(insn, ops, m)
        if m == "sbb":
            return self._lift_sbb(insn, ops)
        if m == "adc":
            return self._lift_adc(insn, ops)
        if m in ("shl", "sal"):
            return self._lift_shift(insn, ops, "<<")
        if m == "shr":
            return self._lift_shift(insn, ops, ">>")
        if m == "sar":
            return self._lift_sar(insn, ops)
        if m in ("rol", "ror"):
            return self._lift_rotate(insn, ops, m)
        if m in ("rcl", "rcr"):
            return self._lift_rotate_carry(insn, ops, m)

        # ── Comparison / test (standalone, not part of cmp+jcc pattern) ──
        if m == "cmp":
            return self._lift_cmp(insn, ops)
        if m == "test":
            return self._lift_test(insn, ops)

        # ── Control flow ──
        if m == "call":
            return self._lift_call(insn, ops)
        if m in ("ret", "retn", "retf"):
            return self._lift_ret(insn, ops)
        if m == "jmp":
            return self._lift_jmp(insn, ops)
        if insn.is_cond_jump:
            return self._lift_jcc(insn)

        # ── String operations ──
        if m.startswith(("rep ", "repe ", "repne ", "repz ", "repnz ")):
            return self._lift_rep_string(insn, m)
        # "movsd" is TWO instructions. The string MOVSD copies a dword from
        # [esi] to es:[edi]; the SSE2 MOVSD moves a scalar double in or out of
        # an xmm register. They share a mnemonic and nothing else, and this
        # branch runs ahead of the SSE one below, so an SSE movsd was lifted
        # as a string copy -- walking esi and edi and touching neither operand
        # the instruction actually names. Told apart by the only thing that
        # differs.
        if m in ("movsb", "movsd", "movsw", "stosb", "stosd", "stosw",
                 "lodsb", "lodsd", "lodsw", "cmpsb", "cmpsw", "cmpsd",
                 "scasb", "scasw", "scasd") and not _has_xmm_operand(ops):
            return self._lift_string_op(insn, m)
        if m == "wait":
            return ["/* wait - FPU sync */"]

        # ── Misc ──
        if m in ("cbw", "cwd", "cwde", "cdq"):
            return _project_compare.extend(self, insn, ops)
        if m == "bswap":
            return self._lift_swap(insn, ops)
        if m in ("bt", "bts", "btr", "btc"):
            return self._lift_bit_test(insn, ops, m)
        # INT 2D is the Xbox kernel debug trap: eax picks the service, ecx
        # carries its argument, and service 1 prints the ANSI_STRING ecx points
        # at. It is always followed by an int3 that the kernel skips over.
        #
        # Emitting nothing for that int3 is not laziness about breakpoints: a
        # __debugbreak() there terminates the process with STATUS_BREAKPOINT,
        # which is exit code 3 and no message at all. Wreckless hit it the
        # first time it tried to print a debug line, after a full boot.
        if m == "int" and ops and ops[0].type == "imm" and ops[0].imm == 0x2D:
            return ["recomp_debug_service(eax, ecx); /* int 0x2d */"]
        if m == "int3":
            # Still stepped over -- a __debugbreak() here kills the process
            # with STATUS_BREAKPOINT and no message, which is how Wreckless
            # died after a full boot. But say so. The slide byte after
            # `int 0x2d` is filtered at run time, so what gets reported is a
            # trap the title genuinely reached, which is nearly always a C++
            # throw this runtime cannot unwind.
            return [f"recomp_int3_reached(0x{insn.address:08X}U);"
                    " /* int3, stepped over */"]
        if m in ("leave",):
            return ["esp = ebp;", "POP32(esp, ebp); /* leave */"]
        if m in ("cld", "std"):
            return _project_strings.direction(self, insn)
        if m == "lahf":
            return ["/* lahf - load AH from flags (used in FPU compare idiom) */"]
        if m == "sahf":
            return ["/* sahf - store AH to flags */"]
        if m == "shld":
            return self._lift_shld(insn, ops)
        if m == "shrd":
            return self._lift_shrd(insn, ops)
        if m == "emms":
            # A statement rather than a comment: emms genuinely has no
            # effect here (mm/x87 aliasing is not modelled), and a
            # comment-only lift is how the conformance suite reports a
            # silently dropped instruction.
            return ["(void)0; /* emms - empty MMX state */"]
        if m in ("xlat", "xlatb"):
            # AL indexes the byte table at EBX. With an address-size override,
            # the effective offset is calculated and wrapped at 16 bits.
            raw_bytes = bytes.fromhex(insn.bytes_hex)
            if 0x67 in raw_bytes[:-1]:
                address = "(uint16_t)(LO16(ebx) + LO8(eax))"
            else:
                address = "ebx + LO8(eax)"
            return [f"SET_LO8(eax, MEM8({address})); /* xlatb */"]
        if m.startswith("set") and "j" + m[3:] in COND_MAP:
            return self._lift_setcc(insn, ops, m)
        if m.startswith("cmov") and "j" + m[4:] in COND_MAP:
            return self._lift_cmovcc(insn, ops, m)

        # ── SSE (scalar float) ──
        # Non-temporal SSE stores differ from the ordinary ones only in
        # cache behaviour, which nothing here models -- but they are
        # stores, and dropping them loses the data. Wreckless writes 50
        # of them in its texture upload path.
        if m in ("movntps", "movntpd", "movntdq"):
            m = "movaps"

        # ── MMX ──
        # Dispatched on the operands rather than the mnemonic, because the
        # integer SIMD names are shared with SSE: `paddw mm0, mm1` and
        # `paddw xmm0, xmm1` differ only in register file.
        # cvtpi2ps is named here as well: its source may be m64 rather than
        # an mm register, and dispatching purely on the operands then sent
        # that form past the MMX lifter to a TODO comment -- the same silent
        # drop, one addressing mode along.
        if any(op.type == "reg" and op.reg and op.reg.startswith("mm")
               and not op.reg.startswith("xmm") for op in ops) or m in (
                   "emms", "femms", "cvtpi2ps"):
            return self._lift_mmx(insn, m, ops)

        if m in _project_sse.MNEMONICS:
            return self._lift_sse(insn, m, ops)

        # ── FPU ──
        if m.startswith("f"):
            return self._lift_fpu(insn, m, ops)

        # ── Explicit carry-flag manipulation ──
        # MSVC emits these around the multi-word arithmetic helpers, and around
        # the "return a bool in CF" idiom. They were unhandled, so the flag the
        # following adc/sbb reads kept whatever the last arithmetic left in it.
        # Only worth emitting when something downstream consumes CF -- that is
        # also the only time the enclosing function declares _cf.
        # Cache hints and store fences: nothing to model on a single-threaded
        # interpreter over coherent host memory. Named so they stop showing up
        # in the unimplemented report as if they were missing work.
        if m.startswith("prefetch") or m in ("sfence", "lfence", "mfence"):
            return [f"(void)0; /* {m}: cache/ordering hint, nothing to model */"]

        if m in ("stc", "clc", "cmc"):
            if not self.needs_cf:
                return [f"/* {m}: no adc/sbb in this function consumes CF */"]
            expr = {"stc": "1", "clc": "0", "cmc": "!_cf"}[m]
            return [f"_cf = {expr}; /* {m} */"]

        # ── Time stamp counter ──
        #
        # The guest reads wall-clock time through it. Xbox's
        # QueryPerformanceCounter is literally `rdtsc`, and its
        # QueryPerformanceFrequency returns the CPU clock as a constant --
        # Half-Life 2's is 0x2BB5C755 (733,333,333 Hz) at 0x0059C6C7. So a
        # frame timer computes seconds as counter / 733333333, and an rdtsc
        # that does nothing leaves the counter fixed: every "now - last" is
        # zero, and a loop waiting for time to pass never finishes. HL2 spins
        # 300 million times in sub_0040F4E0 doing exactly that.
        #
        # The runtime scales the host's counter to the console's clock rate
        # rather than returning the host TSC, so the guest's own division by
        # its hardcoded frequency yields real seconds.
        if m == "rdtsc":
            return [
                "{ uint64_t _tsc = xbox_ReadTimeStampCounter();",
                "  eax = (uint32_t)_tsc; edx = (uint32_t)(_tsc >> 32); }"
                "  /* rdtsc */",
            ]

        if m in ("bsf", "bsr"):
            return self._lift_scan(insn, ops, m)

        if m in ("xadd", "lock xadd", "cmpxchg", "lock cmpxchg"):
            return self._lift_exchange(insn, ops)

        # ── Unhandled ──
        #
        # Recorded, not merely commented -- see self.unimplemented.
        return self._unimplemented(insn, m)

    # ── MOV family ──

    _lift_mov = _project_moves.mov

    _lift_movzx = _project_moves.movzx

    _lift_movsx = _project_moves.movsx

    _lift_lea = _project_moves.lea

    _lift_xchg = _project_exchange.exchange
    _lift_exchange = _project_exchange.exchange

    # ── Stack ──

    _lift_push = _project_moves.push

    _lift_pop = _project_moves.pop

    # ── ALU binary operations ──

    def _result_snapshot(self, ops, m, src_too=False):
        """Publish a result-setter's flags where they are computed.

        The consuming jcc can be several instructions -- or several basic
        blocks -- after the write, and anything between them may have
        replaced the destination: a mov, a pop, a lea, a reloaded loop
        pointer. Reading it back at the branch then asks about the wrong
        value, silently.

        inc/dec already publish _fa for exactly this, and _make_condition
        says why: "Result at the flag-setting instruction, before later
        MOVs." This is that, for the rest of the family.

        src_too captures the SOURCE into _fb instead, for add and sub, whose
        carry and ordered conditions recover the original destination from
        result and source. The caller emits it BEFORE the write.
        """
        size = _operand_width(ops[0])
        if size not in self._SNAP_MASK:
            size = 4
        mask, sx = self._SNAP_MASK[size], self._SNAP_SX[size]
        if src_too:
            src = _fmt_operand_read(ops[1])
            return (f"_fb = (uint32_t)({src}) & {mask};"
                    f" _fbs = (int32_t){sx}(_fb);"
                    f" /* {m} source, before the write */")
        dst = _fmt_operand_read(ops[0])
        return (f"_fa = (uint32_t)({dst}) & {mask};"
                f" _fas = (int32_t){sx}(_fa); /* {m} result */")

    _lift_alu_binop = _project_integer.alu_binop

    _lift_inc_dec = _project_integer.inc_dec

    _lift_neg = _project_integer.neg

    _lift_not = _project_integer.bit_not

    _lift_sbb = _project_integer.sbb

    _lift_adc = _project_integer.adc

    _lift_double_shift = _project_bits.double_shift

    _lift_shld = _project_bits.shld

    _lift_shrd = _project_bits.shrd

    _lift_bit_test = _project_bitops.bit_test
    _lift_scan = _project_bitops.scan
    _lift_swap = _project_bitops.swap
    _lift_imul = _project_product.imul

    _lift_muldiv = _project_product.muldiv

    _lift_shift = _project_bits.shift

    _lift_sar = _project_bits.sar

    _lift_rotate_carry = _project_bits.rotate_carry

    _lift_rotate = _project_bits.rotate

    # ── Compare / Test (standalone) ──

    # Sign-extending cast and mask per operand width, shared by the flag
    # snapshot below and by the arithmetic shift above.
    _SNAP_MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
    _SNAP_SX = {1: "(int8_t)", 2: "(int16_t)", 4: "(int32_t)"}

    _snapshot_flags = _project_compare.snapshot
    _lift_cmp = _project_compare.cmp
    _lift_test = _project_compare.test

    # ── Control flow ──

    def _build_call_args(self, target_addr):
        """Build argument list for a function call based on ABI data."""
        abi_info = self.abi_db.get(target_addr, {})
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)

        args = []
        if cc in ("thiscall", "thiscall_cdecl"):
            args.append("(void*)(uintptr_t)ecx")
        for i in range(num_params):
            args.append(f"0 /* a{i+1} */")
        return ", ".join(args)

    # SEH prolog/epilog addresses - these functions modify ebp for their
    # caller.  After calling __SEH_prolog, the caller must read back ebp
    # from g_seh_ebp.  Before returning, __SEH_prolog writes g_seh_ebp.
    #
    # Per-title addresses, detected from the binary by detect_seh_helpers()
    # and assigned to the instance. The class values are only a fallback for
    # callers that construct a Lifter without a function database.
    SEH_PROLOGS = frozenset()
    SEH_EPILOGS = frozenset()

    # _CxxThrowException, detected from the binary. None when the title
    # never throws.
    CXX_THROW = None

    # The CRT's setjmp/longjmp, detected by detect_setjmp_helpers().
    SETJMP_FN = None
    LONGJMP_FN = None

    callee_pop = _project_control.callee_cleanup

    _lift_call = _project_control.call

    _lift_ret = _project_control.return_from

    _forced_tail = _project_control.forced_tail

    _is_external_target = _project_control.external

    _read_jump_table = _project_control.read_table

    _analyze_switch_table = _project_control.switch_targets

    _lift_jmp = _project_control.jump

    _lift_jcc = _project_control.branch

    # ── SETcc / CMOVcc ──

    _lift_loop = _project_control.loop

    def _lift_setcc(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        return [_fmt_operand_write(ops[0], f"_flags /* {m} */")]

    def _lift_cmovcc(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [f"if (_flags /* {m} */) {_fmt_operand_write(ops[0], src)}"]

    # ── String operations ──

    _lift_rep_string = _project_strings.emit
    _lift_string_op = _project_strings.emit

    # ── FPU (x87) ──

    # ── SSE (scalar/packed float) ──


    _lift_mmx = _project_mmx.emit

    def _unimplemented(self, insn, m, note=""):
        """The one way out for an instruction this lifter cannot translate.

        Until 19 Sep 2026 that way out was a bare `/* TODO: ... */` comment,
        which the C compiler reads as nothing at all: the instruction vanished
        and the guest carried on with whatever the registers held. Nothing at
        runtime could say the site had even been REACHED, so an untranslated
        instruction surfaced as a subsystem failure somewhere else -- the
        Wreckless heap (`bsf`), the Half-Life 2 intro (`cvtpi2ps`) -- and cost
        an afternoon each time. The translator's end-of-run tally lists these
        sites but cannot say which are live: on JSRF 122 sites sit in the
        image and the mnemonics (`bound`, `arpl`, `daa`, `hlt`...) are what a
        linear sweep reads over data, so most are probably dead. Probably.

        So the site now carries RECOMP_UNIMPL(text, va): the runtime logs the
        guest address the first times it is reached, and
        under RECOMP_UNIMPL_TRAP stops there, at the cause, instead of
        somewhere downstream. The comment stays so `grep TODO:` still finds
        every site. The tally in self.unimplemented is recorded here and
        nowhere else, so every emitted marker is also a counted one.
        """
        self.unimplemented.setdefault(m, []).append(insn.address)
        text = f"{m} {insn.op_str}".strip()
        return [f'RECOMP_UNIMPL("{text}", 0x{insn.address:08X}u);'
                f" /* TODO: {text}{note} */"]

    _lift_sse = _project_sse.emit

    # Original emitter uses the shared binary64 stack interface.
    _lift_fpu = _project_x87.emit


def _is_rep_compare(insn):
    """REPE/REPNE CMPS or SCAS -- the prefixed forms that write `_flags`."""
    if not insn.mnemonic.startswith("rep"):
        return False
    text = f"{insn.mnemonic} {getattr(insn, 'op_str', '') or ''}"
    return any(f in text for f in ("cmpsb", "cmpsw", "cmpsd",
                                   "scasb", "scasw", "scasd"))


def lift_basic_block(lifter, bb, flag_state=None):
    """
    Lift a basic block to C statements.
    Tracks flags to generate proper conditions for jcc/setcc/cmovcc.

    Args:
        lifter: Lifter instance
        bb: BasicBlock with instructions
        flag_state: tuple of (flag_setter_mnemonic, flag_operands) from
                    a preceding block, or None

    Returns:
        (stmts, flag_state) where stmts is a list of C statement strings
        and flag_state is a tuple for passing to the next block.
    """
    stmts = []
    insns = bb.instructions
    i = 0

    # Track the last instruction that set flags
    if flag_state:
        last_flag_setter, last_flag_ops = flag_state
    else:
        last_flag_setter = None
        last_flag_ops = []

    while i < len(insns):
        curr = insns[i]

        # Try cmp/test + jcc pattern first (2-instruction match)
        match = try_match_cmp_jcc(insns, i, lifter=lifter)
        if match:
            stmt, consumed = match
            flag_insn = insns[i]
            # The fused form tests the operands inline, but the flags stay live
            # for any later jcc, and that one reads the snapshot. Emit the
            # snapshot here too or those temps are stale - which silently sends
            # every reusing branch the wrong way.
            # Snapshot and record the same normalised form, or a later jcc
            # reads `cmp X, 0` state out of `test X, X` temps.
            last_flag_setter, last_flag_ops = normalise_zero_test(
                flag_insn.mnemonic, list(flag_insn.operands))
            if flag_insn.mnemonic in ("cmp", "test") and len(flag_insn.operands) >= 2:
                stmts.extend(lifter._snapshot_flags(
                    flag_insn, last_flag_ops, last_flag_setter))
            stmts.append(stmt)
            i += consumed
            continue

        # Handle jecxz/jcxz specially (not flag-based)
        if curr.mnemonic in ("jecxz", "jcxz"):
            results = lifter._lift_jcc(curr)
            stmts.extend(results)
            i += 1
            continue

        # LOOP counts ECX down rather than reading a flag. LOOPE/LOOPNE also
        # test ZF, which only has an expression while a tracked instruction
        # still owns the flags.
        if curr.mnemonic in LOOP_JUMPS:
            zf_expr = None
            if curr.mnemonic != "loop" and last_flag_setter:
                probe = _make_condition(
                    "je" if curr.mnemonic == "loope" else "jne",
                    last_flag_setter, last_flag_ops)
                if probe:
                    zf_expr = probe[0]
            stmts.extend(lifter._lift_loop(curr, zf_expr))
            i += 1
            continue

        # Check if this instruction uses flags (jcc, setcc, cmovcc)
        if curr.is_cond_jump and last_flag_setter:
            result = _make_condition(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if result:
                cond_expr, desc = result
                if last_flag_setter in ('comiss', 'ucomiss', 'comisd', 'ucomisd'):
                    # Optional title policy for a float-comparison branch.
                    # Keep captured operands/flags and evaluate the condition once.
                    cond_expr = f"RECOMP_FLOAT_BRANCH_AT(0x{curr.address:X}, ({cond_expr}))"
                target = curr.jump_target
                stmt = _emit_cond_goto(
                    cond_expr, curr.mnemonic, desc, target, lifter)
                stmts.append(stmt)
                i += 1
                continue

        if (curr.mnemonic.startswith("set") and "j" + curr.mnemonic[3:] in COND_MAP
                and last_flag_setter and len(curr.operands) >= 1):
            cond = _make_setcc_value(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                stmts.append(
                    _fmt_operand_write(curr.operands[0],
                                       f"({cond}) ? 1 : 0")
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        # lahf copies SF:ZF:0:AF:0:PF:1:CF into AH. MSVC's float compare
        # with no fcomi/SSE branch of its own is `ucomiss; lahf; test ah, 44h;
        # jnp` (and the 05h/41h variants), so an AH left untouched makes every
        # such branch read whatever eax last held. Outrun 2's draw-sort loop
        # did exactly that and walked into unfilled entries.
        if curr.mnemonic == "lahf" and last_flag_setter:
            ah = _make_lahf_value(last_flag_setter, last_flag_ops)
            if ah:
                stmts.append(f"SET_HI8(eax, {ah}); /* lahf */")
                i += 1
                continue

        if (curr.mnemonic.startswith("cmov") and "j" + curr.mnemonic[4:] in COND_MAP
                and last_flag_setter and len(curr.operands) >= 2):
            cond = _make_cmovcc_cond(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                src = _fmt_operand_read(curr.operands[1])
                stmts.append(
                    f"if ({cond}) "
                    + _fmt_operand_write(curr.operands[0], src)
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        # NEG sets CF when its operand is nonzero. Preserve that value when
        # a later SBB/ADC consumes it, skipping over EFLAGS-preserving
        # instructions (e.g. neg eax; push edi; sbb eax, eax).
        if curr.mnemonic == "neg":
            j = i + 1
            while (j < len(insns)
                    # popfd is no longer in _EFLAGS_PRESERVE, so the set
                    # carries this now; it used to need naming here, which is
                    # how the inconsistency was visible in the first place.
                    and (insns[j].mnemonic in _EFLAGS_PRESERVE
                         or _project_sse.preserves_flags(insns[j])
                         or _project_mmx.preserves_flags(insns[j])
                         or _project_x87.preserves_flags(insns[j]))
                    and not insns[j].is_branch
                    and not insns[j].is_call
                    and not insns[j].is_ret):
                j += 1
            preserve = (j < len(insns)
                        and insns[j].mnemonic in ("sbb", "adc"))
            results = lifter._lift_neg(
                curr, curr.operands, preserve_carry=preserve)
        else:
            results = lifter.lift_instruction(insns[i])
            # A REPE/REPNE compare whose count is zero leaves EFLAGS alone,
            # but `_flags` would keep whatever it last held -- 0 on entry --
            # so the je after it read "not equal" where the hardware reads
            # the ZF of the instruction before. MSVC's basic_string::compare
            # is `xor eax, eax; repe cmpsb; je` with the count = the shorter
            # length, so comparing against an empty string took the wrong
            # arm. Load the incoming ZF first, when a tracked setter has one.
            # (_cf needs nothing: only an iteration writes it.)
            if _is_rep_compare(curr) and last_flag_setter:
                zf = _make_condition("je", last_flag_setter, last_flag_ops)
                if zf:
                    stmts.append(f"_flags = ({zf[0]}) ? 1 : 0;"
                                 " /* ZF in: a zero count keeps it */")
        stmts.extend(results)

        # Track flag-setting instructions.
        #
        # LOCK changes atomicity, not arithmetic: a locked instruction leaves
        # exactly the flags its unlocked form does. Capstone keeps the prefix
        # in the mnemonic, so it is stripped here and everything downstream
        # sees the base instruction it already knows how to read. Matching the
        # prefixed spelling instead loses the flags twice over -- "lock xadd"
        # used to sit in _FLAGS_UNDEFINED and cleared tracking, and
        # "lock cmpxchg" matched no list at all and so left the *previous*
        # instruction's flags standing as if they were its own.
        flag_mnem = (curr.mnemonic[5:] if curr.mnemonic.startswith("lock ")
                     else curr.mnemonic)
        if _project_bits.constant_noop(curr):
            pass  # Retain the previous flag interpretation for a known no-op.
        elif _project_strings.is_compare(curr) and not curr.mnemonic.startswith("rep"):
            last_flag_setter, last_flag_ops = "cmp", list(curr.operands)
        elif flag_mnem in FLAG_SETTERS:
            last_flag_setter, last_flag_ops = normalise_zero_test(
                flag_mnem, list(curr.operands))
        elif flag_mnem in _FLAGS_UNDEFINED:
            # Flags are undefined after these - clear tracking
            last_flag_setter = None
            last_flag_ops = []
        elif flag_mnem in _EFLAGS_SETTERS:
            # Additional flag-setting instructions
            last_flag_setter, last_flag_ops = normalise_zero_test(
                flag_mnem, list(curr.operands))
        elif (curr.mnemonic in _EFLAGS_PRESERVE or _project_sse.preserves_flags(curr)
              or _project_mmx.preserves_flags(curr) or _project_x87.preserves_flags(curr)):
            pass  # These don't affect EFLAGS
        elif curr.mnemonic in ("fcompi", "fcomip", "fucomi", "fucompi",
                                "fucomip", "fcomi"):
            # FPU compare-to-EFLAGS: sets CF, ZF, PF directly
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic == "sahf":
            # sahf loads AH into flags - typically after fnstsw ax
            # in the fcomp/fnstsw/sahf pattern for FPU comparisons
            last_flag_setter = "sahf"
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic.startswith("f") or curr.mnemonic.startswith("cmov"):
            pass  # FPU and already-handled CMOVcc
        elif curr.mnemonic.startswith("j"):
            pass  # Jumps don't set flags
        elif curr.mnemonic.startswith("set"):
            pass  # SETcc doesn't set flags
        elif curr.mnemonic.startswith("rep"):
            # rep movsb/movsd = data copy, preserves flags
            # repe cmpsb/repne scasb = comparison, sets flags
            rest = curr.op_str.strip() if hasattr(curr, 'op_str') else ""
            raw_m = curr.mnemonic
            if "cmps" in raw_m or "scas" in raw_m:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            elif "cmps" in rest or "scas" in rest:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            else:
                pass  # rep movs/stos = data movement, flags preserved
        else:
            # Unknown instruction - conservatively clear flag state
            last_flag_setter = None
            last_flag_ops = []

        i += 1

    out_flag_state = (last_flag_setter, last_flag_ops) if last_flag_setter else None
    return stmts, out_flag_state
