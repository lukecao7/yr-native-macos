"""Regression: argparse errors and crashes must not count as tamper detection."""
import importlib.util
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("round5_gate", root / "local_round5.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
p = subprocess.run([sys.executable, str(root.parent / "p20-world-core/verify_core.py"),
                    "--tamper", "no_step"], capture_output=True, text=True)
assert p.returncode == 2
assert not gate.rejected(p, "RESULT: FAIL")
p = subprocess.run([sys.executable, "-c", "raise RuntimeError('broken verifier')"],
                   capture_output=True, text=True)
assert p.returncode == 1
assert not gate.rejected(p, "RESULT: FAIL")
print("PASS failure gate: unsupported CLI arguments and verifier crashes are not detections")
