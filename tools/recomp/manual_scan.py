"""Compatibility entry for project C-source observations."""
from pathlib import Path
import sys
# Some consumers load this path with importlib without a package context.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.xbe_parser.xbe_parser import XBEParser
from c_source_scan import scan, definition_names, strip_disabled
