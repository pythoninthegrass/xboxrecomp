"""Original installed-translator contract for the architectural frame cell."""
from tools.recomp.translator import FunctionTranslator


def test_prologue_saves_live_frame_before_establishing_its_own():
    raw=bytes.fromhex('558bec8b45005dc3')
    info={'end':0x1000+len(raw),'name':'frame_probe'}
    translator=FunctionTranslator(b'',{0x1000:info})
    translator._read_func_bytes=lambda start,end:raw
    code=translator.translate_function(0x1000,info)
    binding=code.index('uint32_t *const _recomp_live_ebp = &g_ebp;')
    push=code.index('PUSH32(esp, ebp);')
    setup=code.index('ebp = esp;')
    assert binding<push<setup<code.index('eax = MEM32(ebp);')
    assert code.index('POP32(esp, ebp);')<code.index('esp += 4; return; /* ret */')
    assert '#define ebp (*_recomp_live_ebp)' in code
    assert code.rstrip().endswith('#undef ebp')
