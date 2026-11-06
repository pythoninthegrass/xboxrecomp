"""Compatibility module for the project's translation_hooks implementation."""
import sys
from tools.xbe_parser.xbe_parser import XBEParser as _ProjectReader
import translation_hooks as _implementation

sys.modules[__name__] = _implementation
