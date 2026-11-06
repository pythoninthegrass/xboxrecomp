from .loader import BinaryImage  # Initialize the project module search path.
import sys
import analysis_cache
sys.modules[__name__] = analysis_cache
