"""Compatibility entry for project translation reports."""
from tools.xbe_parser.xbe_parser import XBEParser
from generation_output import write_summary, print_stats
from generation_output import generate_header as _generate_header
from . import config

def generate_header(func_list, output_path, abi_db=None, title="Xbox title"):
    return _generate_header(func_list, output_path, abi_db, config.banner_name(title))
