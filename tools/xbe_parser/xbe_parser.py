"""Compatibility entry point for the project-owned XBE reader."""
import os
from pathlib import Path
import sys

# Build drivers provide the source root when reusing another checkout's deps.
explicit = os.environ.get('TSFP_PROJECT_ROOT')
roots = [Path(explicit)] if explicit else list(Path(__file__).resolve().parents)
for root in roots:
    if (root / 'tools/compiler/xbe_image.py').is_file():
        sys.path.insert(0, str(root / 'tools/compiler'))
        break
else:
    raise ImportError('Project XBE reader missing; set TSFP_PROJECT_ROOT to the source checkout')
from xbe_image import XBEParser, XBEError, SECTION_EXECUTABLE, SECTION_WRITABLE, format_flags, export_json, kernel_names, main

if __name__ == '__main__':
    main()
