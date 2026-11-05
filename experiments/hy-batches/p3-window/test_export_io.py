#!/usr/bin/env python3
"""Reject real short writes from both offscreen display-adapter entry points."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile


def limit_file_size():
    resource.setrlimit(resource.RLIMIT_FSIZE, (64, 64))
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)


def main():
    exe = Path("present_test").resolve()
    failures = 0

    def need(name, ok):
        nonlocal failures
        print(f"EXPORT_IO_CASE={name} OK={int(ok)}", flush=True)
        failures += not ok

    def render(mode, path, limited=False):
        return subprocess.run([str(exe), "--export-" + mode, str(path)],
                              capture_output=True, text=True, timeout=30,
                              preexec_fn=limit_file_size if limited else None)

    with tempfile.TemporaryDirectory(prefix="yr-p3-export-io-") as td:
        root = Path(td)
        for mode in ("indexed", "rgb565"):
            normal = root / (mode + ".png")
            p = render(mode, normal)
            original = normal.read_bytes() if normal.exists() else b""
            need(mode + "_normal", p.returncode == 0 and len(original) > 64)
            fresh = root / (mode + "-limited.png")
            p = render(mode, fresh, limited=True)
            need(mode + "_short_write", p.returncode == 1 and
                 "OFFSCREEN_WRITE_OK=0" in p.stdout and not fresh.exists())
            p = render(mode, normal, limited=True)
            need(mode + "_preserves_existing", p.returncode == 1 and
                 "OFFSCREEN_WRITE_OK=0" in p.stdout and normal.read_bytes() == original)
            missing = root / "missing" / (mode + ".png")
            p = render(mode, missing)
            need(mode + "_missing_parent", p.returncode == 1 and not missing.exists())
            p = render(mode, root)
            need(mode + "_directory_target", p.returncode == 1 and root.is_dir())
        need("no_temporary_files", sorted(f.name for f in root.iterdir()) ==
             ["indexed.png", "rgb565.png"])
    print(f"EXPORT_IO_FAILURES={failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
