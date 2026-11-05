#!/usr/bin/env python3
"""Check real offscreen file writes, including a short write in a child process."""
from pathlib import Path
import resource
import signal
import subprocess
import sys
import tempfile

from verify_shell import decode_png


def limit_file_size():
    # Only the child is limited. Ignore SIGXFSZ so writes fail rather than crash.
    resource.setrlimit(resource.RLIMIT_FSIZE, (64, 64))
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)


def main():
    exe = Path(sys.argv[1]).resolve()
    failures = 0

    def need(name, ok, process=None):
        nonlocal failures
        print(f"EXPORT_IO_CASE={name} OK={int(ok)}", flush=True)
        if not ok:
            failures += 1
            if process:
                print(f"exit={process.returncode} stderr={process.stderr.strip()}")

    def render(path, limited=False):
        return subprocess.run(
            [str(exe), "--offscreen", str(path), "32", "24", "7"],
            capture_output=True, text=True, timeout=30,
            preexec_fn=limit_file_size if limited else None)

    with tempfile.TemporaryDirectory(prefix="yr-shell-export-io-") as td:
        root = Path(td)
        normal = root / "normal.png"
        p = render(normal)
        original = normal.read_bytes() if normal.exists() else b""
        decoded = decode_png(normal) if original else None
        need("normal_cli", p.returncode == 0 and len(original) > 64 and
             decoded is not None and decoded[:2] == (32, 24), p)

        fresh = root / "limited.png"
        p = render(fresh, limited=True)
        need("short_write_no_output", p.returncode == 1 and
             "offscreen render failed rc=" in p.stderr and not fresh.exists(), p)

        p = render(normal, limited=True)
        need("short_write_preserves_existing", p.returncode == 1 and
             "offscreen render failed rc=" in p.stderr and
             normal.read_bytes() == original, p)
        need("no_temporary_files", sorted(f.name for f in root.iterdir()) == ["normal.png"])

    print(f"EXPORT_IO_FAILURES={failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
