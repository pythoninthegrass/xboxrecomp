"""Compatibility imports for the project-owned executable analysis view."""
# The parser bridge selects the source checkout, including shared dependency roots.
from tools.xbe_parser.xbe_parser import XBEParser
from recomp_image import (BinaryImage, SectionInfo, KernelImport, DATA_SECTION_NAMES,
                          load_image, _find_analysis_json, source_fingerprint, read_code_window)
from x86_decode import Instruction, classify_instruction, make_decoder
from x86_flow import InstructionTraversal
from x86_recovery import CodeRecovery
