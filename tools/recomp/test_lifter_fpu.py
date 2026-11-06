import unittest
from pathlib import Path

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter
from .translator import FunctionTranslator


class FpuLifterTest(unittest.TestCase):
    def test_stack_register_load_duplicates_the_old_top(self):
        instruction = Instruction(0, 2, "fld", "st(0)", "d9c0")
        instruction.operands = [Operand(type="reg", reg="st(0)")]

        self.assertEqual(
            Lifter().lift_instruction(instruction),
            ['{ /* own fld */', 'double _x87_value = fp_top();', 'fp_push(_x87_value);', '}'],
        )

    def test_fst_does_not_pop_and_fstp_does(self):
        operand = Operand(type="mem", mem_base="esp", mem_disp=0x18,
                          mem_size=4)
        store = Instruction(0, 4, "fst", "dword ptr [esp + 0x18]", "")
        store.operands = [operand]
        store_pop = Instruction(
            0, 4, "fstp", "dword ptr [esp + 0x18]", "")
        store_pop.operands = [operand]

        self.assertEqual(
            Lifter().lift_instruction(store),
            ['{ /* own fst */', 'double _x87_value = fp_top();', 'uint32_t _fp_address = (uint32_t)(esp + 0x18);', 'MEMF(_fp_address) = (float)_x87_value;', '}'],
        )
        self.assertEqual(
            Lifter().lift_instruction(store_pop),
            ['{ /* own fstp */', 'double _x87_value = fp_top();', 'uint32_t _fp_address = (uint32_t)(esp + 0x18);', 'MEMF(_fp_address) = (float)_x87_value;', 'fp_pop();', '}'],
        )

    def test_qword_integer_conversion_uses_signed_64_bit_storage(self):
        operand = Operand(type="mem", mem_base="esp", mem_disp=0x10,
                          mem_size=8)
        store = Instruction(
            0, 4, "fistp", "qword ptr [esp + 0x10]", "")
        store.operands = [operand]
        load = Instruction(0, 4, "fild", "qword ptr [esp + 0x10]", "")
        load.operands = [operand]

        self.assertEqual(
            Lifter().lift_instruction(store),
            ['{ /* own fistp */', 'uint32_t _fp_address = (uint32_t)(esp + 0x10);', 'double _x87_value = fp_top();', 'double _x87_value_rounded = _x87_value;', 'if (isfinite(_x87_value)) {', '    double _fp_integral; double _fp_fraction = modf(_x87_value, &_fp_integral);', '    unsigned _fp_mode = ((g_fp_control_word >> 10) & 3u);', '    double _fp_increment = 0.0;', '    if (_fp_mode == 1u && _fp_fraction < 0.0) _fp_increment = -1.0;', '    if (_fp_mode == 2u && _fp_fraction > 0.0) _fp_increment = 1.0;', '    if (_fp_mode == 0u && (fabs(_fp_fraction) > 0.5 ||', '        (fabs(_fp_fraction) == 0.5 && fmod(fabs(_fp_integral), 2.0) == 1.0)))', '        _fp_increment = copysign(1.0, _fp_fraction);', '    _x87_value_rounded = _fp_integral + _fp_increment;', '    if (_x87_value_rounded == 0.0) _x87_value_rounded = copysign(0.0, _x87_value);', '}', 'uint64_t _fp_integer = (_x87_value_rounded >= -0x1p63 && _x87_value_rounded < 0x1p63)', '    ? (uint64_t)(int64_t)_x87_value_rounded : UINT64_C(0x8000000000000000);', 'MEM64(_fp_address) = (uint64_t)_fp_integer;', 'fp_pop();', '}'],
        )
        self.assertEqual(
            Lifter().lift_instruction(load),
            ['{ /* own fild */', 'uint32_t _x87_value_address = (uint32_t)(esp + 0x10);', 'uint64_t _x87_value_bits = MEM64(_x87_value_address);', 'double _x87_value = (_x87_value_bits & UINT64_C(0x8000000000000000))', '    ? -(double)((~_x87_value_bits + 1u) & UINT64_C(0xFFFFFFFFFFFFFFFF)) : (double)_x87_value_bits;', 'fp_push(_x87_value);', '}'],
        )

    def test_memory_arithmetic_updates_st0_without_popping(self):
        instruction = Instruction(
            0, 6, "fdiv", "dword ptr [0x00123456]", "")
        instruction.operands = [
            Operand(type="mem", mem_disp=0x00123456, mem_size=4),
        ]

        self.assertEqual(
            Lifter().lift_instruction(instruction),
            ['{ /* own fdiv */', 'uint32_t _fp_right_address = (uint32_t)(0x123456);', 'double _fp_right = MEMF(_fp_right_address);', 'double _fp_left = fp_top();', 'fp_top() = _fp_left / _fp_right;', '}'],
        )

    def test_register_arithmetic_updates_st0_without_popping(self):
        instruction = Instruction(0, 2, "fmul", "st(1)", "d8c9")
        instruction.operands = [Operand(type="reg", reg="st(1)")]

        self.assertEqual(
            Lifter().lift_instruction(instruction),
            ['{ /* own fmul */', 'double _fp_right = fp_st1();', 'double _fp_left = fp_top();', 'fp_top() = _fp_left * _fp_right;', '}'],
        )

    def test_translated_functions_share_runtime_fpu_state(self):
        start = 0x00010000
        instructions = [
            Instruction(start, 2, "fld", "st(0)", "d9c0"),
            Instruction(start + 2, 1, "ret", "", "c3"),
        ]
        instructions[0].operands = [Operand(type="reg", reg="st(0)")]
        block = BasicBlock(start=start, instructions=instructions)
        translator = FunctionTranslator(
            b"\0", {start: {"_addr": start, "end": start + 3}})
        translator._read_func_bytes = lambda _start, _end: b"\xd9\xc0\xc3"
        translator.disasm.disassemble_function = (
            lambda _raw, _start, _end: instructions)
        translator.disasm.build_basic_blocks = (
            lambda _instructions, _start, _end, extra_leaders=None: [block])

        generated = translator.translate_function(
            start, {"_addr": start, "end": start + 3})

        self.assertIn("g_fp_stack", generated)
        self.assertIn("g_fp_top", generated)
        self.assertNotIn("double _fp_stack[8]", generated)
        self.assertNotIn("int _fp_top = 0", generated)

    def test_runtime_template_exports_shared_fpu_state(self):
        root = Path(__file__).resolve().parents[2]
        runtime_types = (
            root / "templates" / "runtime" / "recomp_types.h"
        ).read_text()
        main = (
            root / "templates" / "new-game" / "src" / "main.c"
        ).read_text()

        self.assertIn("extern RECOMP_TLS double g_fp_stack[8];", runtime_types)
        self.assertIn("extern RECOMP_TLS int g_fp_top;", runtime_types)
        self.assertIn("#define SMEM64(addr)", runtime_types)
        self.assertIn("extern RECOMP_TLS double g_fp_stack[8];", main)
        self.assertIn("extern RECOMP_TLS int g_fp_top;", main)
        self.assertIn("extern RECOMP_TLS uint16_t g_fp_control_word;", runtime_types)
        self.assertIn("#define RECOMP_PARITY8(x)", runtime_types)
        self.assertIn("extern RECOMP_TLS uint16_t g_fp_control_word;", main)

    def test_control_word_store_and_load_use_shared_state(self):
        operand = Operand(type="mem", mem_base="ebp", mem_disp=0xFFFFFFFC,
                          mem_size=2)
        store = Instruction(0, 3, "fnstcw", "word ptr [ebp - 4]", "")
        store.operands = [operand]
        load = Instruction(0, 3, "fldcw", "word ptr [ebp - 4]", "")
        load.operands = [operand]

        self.assertEqual(
            Lifter().lift_instruction(store),
            ['{ /* own fnstcw */', 'MEM16(ebp + 0xFFFFFFFCu) = g_fp_control_word;', '}'],
        )
        self.assertEqual(
            Lifter().lift_instruction(load),
            ['{ /* own fldcw */', 'g_fp_control_word = MEM16(ebp + 0xFFFFFFFCu);', '}'],
        )

    def test_status_word_store_reports_the_compare_result(self):
        instruction = Instruction(0, 2, "fnstsw", "ax", "dfe0")
        instruction.operands = [Operand(type="reg", reg="ax")]

        lifted = ["\n".join(Lifter().lift_instruction(instruction))]

        self.assertEqual(len(lifted), 1)
        # The compare can live in a different lifted body than the FNSTSW that
        # reads it, so the result has to be shared state, not a function local.
        self.assertIn("g_fp_cc", lifted[0])
        self.assertNotIn("_fpu_cmp", lifted[0])
        # C3 (equal) and C0 (less) at their status-word positions, plus TOP
        self.assertIn("(g_fp_top & 7u) << 11", lifted[0])
        self.assertNotIn("/* fnstsw ax - store FPU status word */", lifted[0])

    def test_compare_writes_the_shared_result(self):
        instruction = Instruction(0, 4, "fcomp", "qword ptr [ebp + 8]", "dc5d08")
        instruction.operands = [
            Operand(type="mem", mem_base="ebp", mem_disp=8, mem_size=8)
        ]

        lifted = ["\n".join(Lifter().lift_instruction(instruction))]

        self.assertEqual(len(lifted), 1)
        self.assertIn("g_fp_cmp =", lifted[0])
        self.assertEqual(lifted[0].count("MEMD("), 1)
        self.assertLess(lifted[0].index("g_fp_cmp ="), lifted[0].index("fp_pop();"))
        self.assertNotIn("_fpu_cmp", lifted[0])

    def test_fxch_swaps_with_the_explicit_register(self):
        """Capstone reports fxch with both operands -- (st(0), st(i)) -- and it
        is the only x87 form that does. Reading operand 0 picked up the
        implicit st(0), so every `fxch st(i)` emitted a swap of st0 with
        itself and silently did nothing."""
        instruction = Instruction(0, 2, "fxch", "st(1)", "d9c9")
        instruction.operands = [Operand(type="reg", reg="st(0)"),
                                Operand(type="reg", reg="st(1)")]

        lifted = "\n".join(Lifter().lift_instruction(instruction))

        self.assertIn("fp_st1()", lifted)
        self.assertNotIn("fp_top() = fp_top()", lifted)


if __name__ == "__main__":
    unittest.main()
