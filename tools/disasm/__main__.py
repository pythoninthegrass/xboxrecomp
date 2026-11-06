from .loader import BinaryImage  # Initialize the project module search path.
from analysis_runner import main, _load_seed_functions

if __name__ == "__main__":
    raise SystemExit(main())
