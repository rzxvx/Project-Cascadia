"""Complete actual-C Lightning decoder with original semantic oracle; offline."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).resolve().parents[1]/"experiments/p105-accessory-id/test-id.py"),run_name="__main__")
