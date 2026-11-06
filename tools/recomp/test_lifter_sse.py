import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _insn(mnemonic, op_str, operands):
    instruction = Instruction(0, 4, mnemonic, op_str, "")
    instruction.operands = operands
    return instruction


def _xmm(name):
    return Operand(type="reg", reg=name)


def _mem(base=None, disp=0, size=16):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=size)


class SsePackedLifterTest(unittest.TestCase):
    """Production routing and memory/lane structure; compiled oracles live in
    the project's test_translation_sse.py and execute the emitted statements."""

    def code(self, mnemonic, operands):
        code = "\n".join(Lifter().lift_instruction(_insn(mnemonic, "", operands)))
        self.assertNotIn("TODO", code)
        self.assertNotIn("RECOMP_UNIMPL", code)
        return code

    def memory_width(self, code, words, store=False):
        self.assertEqual(code.count("MEM32("), words)
        for lane in range(words):
            name = "_simd_dest" if store else "_simd_b_address"
            self.assertIn(f"MEM32({name} + {lane * 4}u)", code)

    def test_aligned_packed_load_transfers_sixteen_bytes(self):
        code = self.code("movaps", [_xmm("xmm0"), _mem("eax")])
        self.memory_width(code, 4)
        self.assertIn("xmm0 = _simd_b;", code)

    def test_aligned_packed_store_transfers_sixteen_bytes(self):
        code = self.code("movaps", [_mem("ecx"), _xmm("xmm0")])
        self.memory_width(code, 4, True)
        self.assertLess(code.index("_simd_b = xmm0"), code.index("MEM32("))

    def test_unaligned_packed_move_is_also_sixteen_bytes(self):
        code = self.code("movups", [_mem("ecx", 0x10), _xmm("xmm1")])
        self.memory_width(code, 4, True)
        self.assertIn("_simd_dest = (uint32_t)(ecx + 0x10)", code)

    def test_scalar_move_stays_on_lane_zero(self):
        code = self.code("movss", [_xmm("xmm0"), _mem("eax", 4, size=4)])
        self.memory_width(code, 1)
        self.assertIn("_simd_result.q[0] = _simd_result.q[1] = 0;", code)
        self.assertIn("_simd_result.u[0] = _simd_b.u[0];", code)
        code = self.code("movss", [_mem("ecx", size=4), _xmm("xmm2")])
        self.memory_width(code, 1, True)
        self.assertIn("_simd_b = xmm2", code)

    def test_packed_arithmetic_emits_a_statement(self):
        for op, symbol in (("add", "+"), ("sub", "-"), ("mul", "*"), ("div", "/")):
            code = self.code(op + "ps", [_xmm("xmm0"), _xmm("xmm1")])
            for lane in range(4):
                self.assertIn(f"_simd_result.f[{lane}] = _simd_a.f[{lane}] {symbol} _simd_b.f[{lane}];", code)

    def test_packed_arithmetic_against_memory(self):
        code = self.code("mulps", [_xmm("xmm0"), _mem(disp=0xa24190)])
        self.memory_width(code, 4)
        self.assertEqual(code.count(" * _simd_b.f["), 4)

    def test_half_width_moves_target_one_half(self):
        code = self.code("movlps", [_xmm("xmm0"), _mem("ecx", size=8)])
        self.memory_width(code, 2)
        self.assertIn("_simd_result = xmm0", code)
        self.assertNotIn("_simd_result.u[2] =", code)
        code = self.code("movhps", [_mem("eax", 8, size=8), _xmm("xmm1")])
        self.memory_width(code, 2, True)
        self.assertIn("_simd_b.q[0] = _simd_b.q[1];", code)

    def test_scalar_arithmetic_reads_and_writes_lane_zero(self):
        code = self.code("mulss", [_xmm("xmm0"), _xmm("xmm1")])
        self.assertIn("RECOMP_F32_MUL_AT(0x0, _simd_a.f[0], _simd_b.f[0])", code)
        self.assertEqual(code.count("_simd_result.f["), 1)
        self.assertIn("_simd_result = _simd_a;", code)

    def test_bitwise_ops_use_the_integer_lanes(self):
        for name, symbol in (("andps", "&"), ("orps", "|"), ("xorps", "^")):
            code = self.code(name, [_xmm("xmm0"), _xmm("xmm1")])
            for lane in range(4):
                self.assertIn(f"_simd_a.u[{lane}] {symbol} _simd_b.u[{lane}]", code)

    def test_self_xor_zeroes_the_whole_register(self):
        code = self.code("xorps", [_xmm("xmm0"), _xmm("xmm0")])
        self.assertIn("_simd_a = xmm0", code)
        self.assertIn("_simd_b = xmm0", code)
        self.assertEqual(code.count(" ^ _simd_b.u["), 4)

    def test_shuffle_uses_the_immediate(self):
        code = self.code("shufps", [_xmm("xmm0"), _xmm("xmm1"), Operand(type="imm", imm=0x55)])
        for lane, source in enumerate(("a", "a", "b", "b")):
            self.assertIn(f"_simd_result.u[{lane}] = _simd_{source}.u[1];", code)

    def test_move_mask_gathers_real_sign_bits(self):
        code = self.code("movmskps", [Operand(type="reg", reg="eax"), _xmm("xmm1")])
        for lane in range(4):
            self.assertIn(f"xmm1.u[{lane}] >> 31", code)
        self.assertIn("eax = ", code)

    def test_no_packed_mnemonic_falls_through_to_a_bare_comment(self):
        names = "addps subps mulps divps minps maxps andps andnps orps xorps unpcklps unpckhps cmpeqps cmpneqps cmpltps cmpleps sqrtps rsqrtps rcpps"
        for name in names.split():
            code = self.code(name, [_xmm("xmm0"), _xmm("xmm1")])
            self.assertIn("xmm0 = _simd_result;", code)
            self.assertIn("_simd_result.", code)


class CvtPi2PsTest(unittest.TestCase):
    """cvtpi2ps: two signed dwords out of an MMX register (or m64) become two
    singles in the LOW half of an xmm, upper lanes untouched.

    It has an xmm destination and an mm source, so it matched neither the mm
    paths nor the xmm ones and fell through to a TODO comment -- which deletes
    the instruction silently. In Half-Life 2's Xbox loader that removed every
    integer-to-float step of the XMV YUV-to-RGB converter; the float pipeline
    then ran on whatever the xmm registers held from before and painted the
    whole intro solid red. A dropped instruction is the worst kind of bug the
    recompiler can have, because everything downstream still runs.
    """

    def test_mmx_source_is_lifted(self):
        lifted = Lifter().lift_instruction(
            _insn("cvtpi2ps", "xmm0, mm0", [_xmm("xmm0"), _xmm("mm0")]))
        self.assertEqual(len(lifted), 1)
        self.assertIn("XMM_FROM_PI(xmm0, mm0)", lifted[0])

    def test_memory_source_is_lifted(self):
        lifted = Lifter().lift_instruction(
            _insn("cvtpi2ps", "xmm1, qword ptr [eax]",
                  [_xmm("xmm1"), _mem("eax", size=8)]))
        self.assertEqual(len(lifted), 1)
        self.assertIn("MMX_MEM(", lifted[0])

    def test_it_does_not_become_a_comment(self):
        # The regression itself: a TODO comment compiles and does nothing.
        statement = Lifter().lift_instruction(
            _insn("cvtpi2ps", "xmm0, mm0", [_xmm("xmm0"), _xmm("mm0")]))[0]
        self.assertNotIn("TODO", statement)
        self.assertFalse(statement.lstrip().startswith("/*"), statement)


if __name__ == "__main__":
    unittest.main()
