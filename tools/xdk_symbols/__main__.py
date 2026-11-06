"""Compatibility entry for the project symbol-database adapter."""
from pathlib import Path
import sys
from ..disasm.loader import BinaryImage
import xdk_catalog as _implementation

_implementation.REPO = Path(__file__).resolve().parents[2]
if __name__ == '__main__':
    raise SystemExit(_implementation.main())
sys.modules[__name__] = _implementation
