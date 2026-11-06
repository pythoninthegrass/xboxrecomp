"""Compatibility entry for the project generation driver."""
from pathlib import Path
import sys
_runtime = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_runtime))
from tools.disasm.loader import BinaryImage
import compile_pipeline as _implementation
from compile_pipeline import main, xbe_title_id, build_commands, run, STAGES

_implementation.REPO = _runtime
if __name__ == '__main__':
    raise SystemExit(_implementation.main())
sys.modules[__name__] = _implementation
