from tools.xbe_parser.xbe_parser import XBEParser as _ProjectXBEParser
from pathlib import Path as _Path
import sys as _sys
import translation_function as _project
_project.BatchTranslator.runtime_root = str(_Path(__file__).resolve().parents[2])
_sys.modules[__name__] = _project
