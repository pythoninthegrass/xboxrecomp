from tools.xbe_parser.xbe_parser import XBEParser as _ProjectXBEParser
from pathlib import Path as _Path
import sys as _sys
import translation_cli as _project
_project.DEFAULT_RUNTIME = str(_Path(__file__).resolve().parents[2])
if __name__ == '__main__':
    raise SystemExit(_project.main())
_sys.modules[__name__] = _project
