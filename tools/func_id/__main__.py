"""Compatibility CLI for the project function catalog."""
from ..disasm.loader import BinaryImage
from function_catalog import main

if __name__ == "__main__":
    raise SystemExit(main())
