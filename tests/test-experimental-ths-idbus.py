"""Source-only actual THS transport and normalized native packet fixtures."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).resolve().parents[1] /
                   "experiments/p105-ths-idbus/test-transport.py"), run_name="__main__")
