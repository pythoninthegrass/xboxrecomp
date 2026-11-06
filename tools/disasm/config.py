from .loader import BinaryImage  # Initialize the project module search path.
import sys
import analysis_config
sys.modules[__name__] = analysis_config
