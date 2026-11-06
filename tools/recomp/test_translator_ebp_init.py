"""Original frame-binding checks; native behavior lives in test_canonical_ebp."""
import pytest
from tools.recomp.translator import FunctionTranslator


@pytest.mark.parametrize('raw',[
    '558bec5dc3',       # Establish and restore a frame.
    '8b450833c05dc3',   # Read the incoming frame without establishing one.
    'bd443322115dc3',   # Use EBP as an ordinary architectural register.
])
def test_register_storage_is_live_and_never_reinitialized_from_legacy_cache(raw):
    data=bytes.fromhex(raw)
    info={'end':0x1000+len(data),'name':'live_frame_probe'}
    translator=FunctionTranslator(b'',{0x1000:info})
    translator._read_func_bytes=lambda start,end:data
    code=translator.translate_function(0x1000,info)
    binding='uint32_t *const _recomp_live_ebp = &g_ebp;'
    assert code.count(binding)==1
    assert code.index(binding)<code.index('POP32(esp, ebp);')
    assert 'uint32_t ebp' not in code
    assert 'ebp = g_ebp;' not in code
    assert 'ebp = g_seh_ebp;' not in code
    assert code.count('#define ebp (*_recomp_live_ebp)')==1
    assert code.count('#undef ebp')==1
