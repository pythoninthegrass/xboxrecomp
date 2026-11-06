"""Compatibility entry point for the project-owned XBE reader."""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'compiler'))
from xbe_image import XBEParser, XBEError, SECTION_EXECUTABLE, SECTION_WRITABLE, format_flags, export_json, kernel_names, main

if __name__ == '__main__':
    main()
