#!/usr/bin/env python3
"""Run isolated prototype tests with generated data, without a game installation."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", action="store_true", help="Also run macOS offscreen adapters; never launch a GUI")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.native and platform.system() != "Darwin":
        parser.error("--native requires macOS")
    suites = ["p2-map", "p5-isomap", "p20-world-core", "p21-input-bridge"]
    if platform.system() == "Darwin":
        suites.insert(0, "p1-mix")  # Uses Apple's CommonCrypto for hashes.
    if args.native:
        suites += ["p3-window", "p22-app-shell"]
    result = {"fixture_origin": "SELF_AUTHORED", "gui_launched": False,
              "original_game_parity_validated": False, "suites": []}
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    with tempfile.TemporaryDirectory(prefix="yr-hy-selftest-") as td:
        work = Path(td) / "experiments"
        shutil.copytree(ROOT, work, ignore=shutil.ignore_patterns("__pycache__", ".generated"))
        tiles = [{"x": x, "y": y, "tile": (x + 3*y) % 47,
                  "sub": 0, "level": 0, "ice": 0}
                 for y in range(80) for x in range(159)]
        (work / "p5-isomap/tiles.json").write_text(json.dumps({
            "width": 159, "height": 80, "cell_count": len(tiles),
            "theater": "TEMPERATE", "source": "SELF_AUTHORED_GRID_159x80", "tiles": tiles}))
        (work / "p20-world-core/unittem-ra2.pal").write_bytes(bytes(i % 64 for i in range(768)))
        for suite in suites:
            p = subprocess.run(["make", "check"], cwd=work/suite, env=env,
                               capture_output=True, text=True, timeout=180)
            result["suites"].append({"name": suite, "exit_code": p.returncode})
            if p.returncode:
                print((p.stdout+p.stderr)[-7000:], file=sys.stderr)
                break
            print("PASS", suite, flush=True)
        world = work / "p20-world-core"
        if (world / "consistency.json").exists():
            result["world_consistency"] = json.loads((world/"consistency.json").read_text())
            snapshot = json.loads((world/"snapshot_1t.json").read_text())
            result["distinct_unit_positions"] = len({(u["x"], u["y"]) for u in snapshot["units"]})
    ok = len(result["suites"]) == len(suites) and all(s["exit_code"] == 0 for s in result["suites"])
    result["passed"] = ok
    if args.report:
        args.report.write_text(json.dumps(result, indent=2)+"\n")
    print("SELF_AUTHORED_RESULT=" + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
