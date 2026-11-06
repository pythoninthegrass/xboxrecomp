"""Compatibility entry for the project-owned branch_feedback tool."""
import sys
from pathlib import Path
if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.xbe_parser.xbe_parser import XBEParser as _ProjectReader
import branch_feedback as _implementation
if __name__ == '__main__':
    raise SystemExit(_implementation.main())
sys.modules[__name__] = _implementation
