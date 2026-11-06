"""Compatibility import for the project-owned instruction lifter context."""
import sys
from tools.xbe_parser.xbe_parser import XBEParser as _ProjectReader
import translation_context as _implementation

sys.modules[__name__] = _implementation
