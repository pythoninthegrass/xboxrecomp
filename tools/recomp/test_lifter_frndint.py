"""FRNDINT must round under the guest's x87 rounding control, not the host's.

It was lifted as `rint(fp_top())`, which rounds in the HOST's mode -- nearest,
always, because the guest's `fldcw` only ever reaches g_fp_control_word. MSVC's
floor() and ceil() are `_ctrlfp(RC=down/up); _frnd(); _ctrlfp(old)`, and
_frnd is a bare frndint, so both of them rounded to nearest: floor of 2.7 was 3.
A title that indexes a keyframe table with floor() and ceil() then reads one
key past the end once per animation loop.

The emitted text is checked here, and emitted instructions run: a text
check cannot tell a correct nearest-even from one that rounds halves up.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction, Operand
from .lifter import Lifter

_ROOT = Path(__file__).resolve().parents[2]


class FrndintLiftTest(unittest.TestCase):
    def test_frndint_reads_the_guest_control_word(self):
        insn = Instruction(0x0017EEDB, 2, "frndint", "", "")
        insn.operands = []
        out = "\n".join(Lifter().lift_instruction(insn))
        self.assertIn("((g_fp_control_word >> 10) & 3u)", out)
        self.assertIn("fp_top() = _x87_value_rounded;", out)
        self.assertIn("modf(_x87_value, &_fp_integral)", out)
        self.assertNotIn("rint(fp_top())", out.replace("recomp_frndint(fp_top()", ""))


def rounding_source():
    """Keep the original boundary oracle, now testing production x87 emission."""
    rounding = Instruction(0, 2, "frndint", "", "")
    rounding.operands = []
    conversion = Instruction(0, 2, "fist", "dword ptr [0]", "")
    conversion.operands = [Operand(type="mem", mem_size=4)]
    wrappers = r"""
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word;
static uint32_t integer_result[1];
#define fp_top() g_fp_stack[g_fp_top]
static double round_value(double value, uint16_t control) {
    fp_top() = value; g_fp_control_word = control;
/* ROUND */
    return fp_top();
}
#undef MEM32
#define MEM32(address) integer_result[(address) / 4u]
static int64_t integer_value(double value, uint16_t control, unsigned bits) {
    (void)bits;
    fp_top() = value; g_fp_control_word = control;
/* CONVERT */
    return (int64_t)(int32_t)integer_result[0];
}
"""
    wrappers = wrappers.replace('/* ROUND */', '\n'.join(Lifter().lift_instruction(rounding)))
    wrappers = wrappers.replace('/* CONVERT */', '\n'.join(Lifter().lift_instruction(conversion)))
    return r'''
#include <stdint.h>
#include <math.h>
#include "recomp_types.h"
/* EMITTED_OPERATIONS */

ptrdiff_t g_xbox_mem_offset;

#define NEAR 0x037Fu   /* the x87 default: RC=00 */
#define DOWN 0x077Fu   /* RC=01, what floor() loads */
#define UP   0x0B7Fu   /* RC=10, what ceil() loads */
#define CHOP 0x0F7Fu   /* RC=11 */

int main(void) {
    /* The defect: floor and ceil of a fraction above one half. */
    if (round_value(59.6, DOWN) != 59.0) return __LINE__;
    if (round_value(59.6, UP)   != 60.0) return __LINE__;
    if (round_value(59.4, UP)   != 60.0) return __LINE__;
    if (round_value(-2.5, DOWN) != -3.0) return __LINE__;
    if (round_value(-2.5, UP)   != -2.0) return __LINE__;
    if (round_value(-2.7, CHOP) != -2.0) return __LINE__;
    if (round_value(2.7, CHOP)  !=  2.0) return __LINE__;
    /* Nearest-even: halves go to the even neighbour. */
    if (round_value(2.5, NEAR)  !=  2.0) return __LINE__;
    if (round_value(3.5, NEAR)  !=  4.0) return __LINE__;
    if (round_value(-1.5, NEAR) != -2.0) return __LINE__;
    if (round_value(2.4, NEAR)  !=  2.0) return __LINE__;
    /* Integral values, huge values and non-finite values pass through. */
    if (round_value(60.0, DOWN) != 60.0) return __LINE__;
    if (round_value(60.0, UP)   != 60.0) return __LINE__;
    if (round_value(1e300, UP) != 1e300) return __LINE__;
    if (!isnan(round_value((double)NAN, DOWN))) return __LINE__;
    if (round_value((double)INFINITY, CHOP) != (double)INFINITY) return __LINE__;
    /* A zero result keeps the operand's sign. */
    if (!signbit(round_value(-0.3, NEAR))) return __LINE__;
    if (!signbit(round_value(-0.7, UP)))   return __LINE__;
    if (signbit(round_value(0.3, DOWN)))   return __LINE__;
    /* FIST shares the rounding, and keeps its integer indefinite. */
    if (integer_value(59.6, DOWN, 32) != 59) return __LINE__;
    if (integer_value(59.6, NEAR, 32) != 60) return __LINE__;
    if (integer_value((double)NAN, NEAR, 32) != INT32_MIN) return __LINE__;
    return 0;
}'''.replace('/* EMITTED_OPERATIONS */', wrappers)


class FrndintRuntimeTest(unittest.TestCase):
    def test_each_rounding_control_rounds_its_own_way(self):
        cc = next(filter(None, map(shutil.which, ("cc", "gcc", "clang"))), None)
        if not cc:
            self.skipTest("no C compiler available")
        source = rounding_source()
        with tempfile.TemporaryDirectory(prefix="frndint-runtime-") as tmp:
            tmp = Path(tmp)
            src, exe = tmp / "test.c", tmp / "test"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(src), "-o", str(exe)]
                    + ([] if os.name == "nt" else ["-lm"]),
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(0, ran.returncode,
                             f"check on line {ran.returncode} failed")


if __name__ == "__main__":
    unittest.main()
