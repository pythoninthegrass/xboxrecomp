"""The x87 reverse-operand, integer-memory and transcendental forms.

None of these had a case in _lift_fpu, so every one of them fell through to
the bare-comment catch-all and executed as nothing at all. The stack depth
stayed balanced while the value did not, so the wrong number flowed forward
silently. fdivr against the constant 1.0 is the reciprocal inside the
vector-normalize helper, which is why the view matrix read back scaled by
the square of its own length instead of unit-length.
"""
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _mem(base=None, disp=0, size=4):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=size)


def _lift(mnemonic, op_str, operands):
    instruction = Instruction(0, 4, mnemonic, op_str, "")
    instruction.operands = operands
    return [" ".join(Lifter().lift_instruction(instruction))]


class FpuReverseFormTest(unittest.TestCase):
    def test_fdivr_divides_the_operand_by_the_stack_top(self):
        """1.0 fdivr len is the reciprocal, not len / 1.0."""
        lifted = _lift("fdivr", "dword ptr [0x281250]", [_mem(disp=0x281250)])

        self.assertEqual(len(lifted), 1)
        self.assertIn("fp_top() = _fp_right / _fp_left;", lifted[0])
        self.assertNotIn("/* FPU:", lifted[0])

    def test_fsubr_subtracts_the_stack_top_from_the_operand(self):
        lifted = _lift("fsubr", "dword ptr [eax + 0x10]",
                       [_mem(base="eax", disp=0x10)])

        self.assertIn("fp_top() = _fp_right - _fp_left;", lifted[0])

    def test_reverse_pop_forms_use_the_reversed_operand_order(self):
        self.assertIn("fp_st1() = _fp_right / _fp_left; fp_pop();",
                      _lift("fdivrp", "st(1)", [])[0])
        self.assertIn("fp_st1() = _fp_right - _fp_left; fp_pop();",
                      _lift("fsubrp", "st(1)", [])[0])

    def test_forward_forms_keep_their_original_operand_order(self):
        """The reverse cases must not disturb the plain ones."""
        self.assertIn("fp_top() = _fp_left / _fp_right;",
                      _lift("fdiv", "dword ptr [eax]", [_mem(base="eax")])[0])
        self.assertIn("fp_top() = _fp_left - _fp_right;",
                      _lift("fsub", "dword ptr [eax]", [_mem(base="eax")])[0])

    def test_integer_memory_operands_are_read_as_signed_integers(self):
        lifted = _lift("fidiv", "dword ptr [esp + 8]",
                       [_mem(base="esp", disp=8)])

        self.assertIn("? -(double)((~_fp_right_bits + 1u) & UINT64_C(0xFFFFFFFF)) : (double)_fp_right_bits;", lifted[0])
        self.assertIn("MEM32(_fp_right_address)", lifted[0])
        self.assertIn("fp_top() = _fp_left / _fp_right;", lifted[0])

        word = _lift("fisubr", "word ptr [ebx + 0xbc]",
                     [_mem(base="ebx", disp=0xBC, size=2)])
        self.assertIn("? -(double)((~_fp_right_bits + 1u) & UINT64_C(0xFFFF)) : (double)_fp_right_bits;",
                      word[0])
        self.assertIn("MEM16(_fp_right_address)", word[0])
        self.assertIn("fp_top() = _fp_right - _fp_left;", word[0])

    def test_transcendentals_emit_statements_rather_than_comments(self):
        for mnemonic, expected in (
            ("fpatan", "atan2(_fp_next, _fp_top)"),
            ("fsin", "sin(_x87_value)"),
            ("fcos", "cos(_x87_value)"),
            ("frndint", "((g_fp_control_word >> 10) & 3u)"),
            ("fyl2x", "log2(_fp_top)"),
        ):
            with self.subTest(mnemonic=mnemonic):
                lifted = _lift(mnemonic, "", [])
                self.assertIn(expected, lifted[0])
                self.assertNotIn("/* FPU:", lifted[0])

    def test_fptan_pushes_the_implicit_one(self):
        """FPTAN leaves tan(x) and then 1.0 on the stack."""
        self.assertIn("fp_push(1.0);", _lift("fptan", "", [])[0])


if __name__ == "__main__":
    unittest.main()
