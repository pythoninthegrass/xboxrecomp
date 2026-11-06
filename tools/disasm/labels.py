"""Compatibility exports for project-owned analysis components."""
from .loader import BinaryImage  # Initialize the project source-root bridge.
from recomp_symbols import (Label, LabelType, LabelManager, populate_kernel_labels,
                            populate_entry_point, extract_strings, populate_string_labels)
