# SPDX-License-Identifier: GPL-3.0-only
"""Exercise windowless JSON Lines with the repository's generated fixture."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


PREFIX = "COASTAL_SCRIPT "


def script(*commands):
    return "".join(json.dumps(dict(id=index, **command)) + "\n"
                   for index, command in enumerate(commands, start=1))


def replay(program, fixture, output, source, commands=None):
    run = subprocess.run(
        [str(program), "--data-dir", str(fixture), "--skirmish-map", "Fixture.map",
         "--script-file", str(source), "--script-output", str(output)],
        input=commands, capture_output=True, text=True, timeout=15,
    )
    rows = [json.loads(line[len(PREFIX):]) for line in run.stdout.splitlines() if line.startswith(PREFIX)]
    return run, rows


def validate(program, work):
    fixture = work / "fixture"
    generator = Path(__file__).with_name("macos_host_fixture_data.py")
    subprocess.run([sys.executable, str(generator), str(fixture)], check=True, timeout=10)

    commands = script(
        {"op": "state"},
        {"op": "frame", "count": 1, "capture": "frames/initial.png"},
        {"op": "pointer", "phase": "down", "x": 420, "y": 280},
        {"op": "pointer", "phase": "up", "x": 420, "y": 280},
        {"op": "key", "phase": "down", "code": 49, "chars": " "},
        {"op": "key", "phase": "up", "code": 49, "chars": " "},
        {"op": "frame", "count": 2, "capture": "frames/updated.png"},
        {"op": "capture", "capture": "repeat.png"},
        {"op": "scroll", "x": 420, "y": 280, "deltaY": -4.5, "modifiers": 1 << 18, "precise": True},
        {"op": "frame", "count": 1, "capture": "frames/scrolled.png"},
        {"op": "scroll", "x": 420, "y": 280, "deltaY": -4.5, "scrollingDeltaY": 0,
         "modifiers": 1 << 18, "phase": 1, "momentumPhase": 4, "inverted": True},
        {"op": "frame", "count": 1, "capture": "frames/zero-scroll.png"},
        {"op": "quit"},
        {"op": "unreachable"},
    )
    command_file = work / "replay.jsonl"
    command_file.write_text(commands, encoding="utf-8")
    file_output, stdin_output = work / "file-output", work / "stdin-output"
    from_file, file_rows = replay(program, fixture, file_output, command_file)
    from_stdin, stdin_rows = replay(program, fixture, stdin_output, "-", commands)
    assert from_file.returncode == from_stdin.returncode == 0, (from_file.stderr, from_stdin.stderr)
    assert file_rows == stdin_rows and len(file_rows) == 13, (file_rows, stdin_rows)
    assert all(row["ok"] and row["id"] == index for index, row in enumerate(file_rows, start=1))
    assert file_rows[0]["frameIndex"] is None and file_rows[0]["viewportWidth"] == 1024
    assert file_rows[1]["advanced"] == 1 and file_rows[1]["frameIndex"] == 0
    assert [row["frameIndex"] for row in file_rows[2:6]] == [0] * 4
    assert file_rows[6]["advanced"] == 2 and file_rows[6]["frameIndex"] == 2
    assert file_rows[7]["frameIndex"] == 2
    assert [row["frameIndex"] for row in file_rows[8:12]] == [2, 3, 3, 4]
    initial = (file_output / "frames/initial.png").read_bytes()
    updated = (file_output / "frames/updated.png").read_bytes()
    assert initial.startswith(b"\x89PNG\r\n\x1a\n") and initial != updated
    assert updated == (file_output / "repeat.png").read_bytes()
    assert initial == (stdin_output / "frames/initial.png").read_bytes()
    assert updated == (stdin_output / "frames/updated.png").read_bytes()
    scrolled = (file_output / "frames/scrolled.png").read_bytes()
    assert scrolled != updated, "Scroll input did not change the fixture zoom."
    assert scrolled == (file_output / "frames/zero-scroll.png").read_bytes()
    assert scrolled == (stdin_output / "frames/scrolled.png").read_bytes()
    assert scrolled == (stdin_output / "frames/zero-scroll.png").read_bytes()

    pointer_only = script(
        {"op": "frame", "count": 1},
        {"op": "pointer", "phase": "down", "x": 420, "y": 280},
        {"op": "pointer", "phase": "up", "x": 420, "y": 280},
        {"op": "frame", "count": 2, "capture": "end.png"},
        {"op": "quit"},
    )
    idle = script({"op": "frame", "count": 3, "capture": "end.png"}, {"op": "quit"})
    control_frames = []
    for name, control in (("pointer-only", pointer_only), ("idle", idle)):
        output = work / name
        run, rows = replay(program, fixture, output, "-", control)
        assert run.returncode == 0 and all(row["ok"] for row in rows), (run.stderr, rows)
        assert rows[-1]["frameIndex"] == 2
        control_frames.append((output / "end.png").read_bytes())
    assert control_frames[0] != control_frames[1], "Pointer input left the fixture unchanged."
    assert updated != control_frames[0], "Keyboard input left the fixture unchanged."

    rejected_output = work / "rejected-output"
    invalid = script(
        {"op": "frame", "count": 1, "capture": "../outside.png"},
        {"op": "capture", "capture": "premature.png"},
        {"op": "frame", "count": 1, "capture": "first.png"},
        {"op": "capture", "capture": "first.png"},
        {"op": "scroll", "x": 420, "deltaY": 6},
        {"op": "scroll", "x": 420, "y": 280, "scrollingDeltaY": "6"},
        {"op": "scroll", "x": 420, "y": 280, "momentumPhase": 1 << 32},
        {"op": "scroll", "x": 420, "y": 280, "precise": 1},
        {"op": "quit"},
    )
    rejected, bad_rows = replay(program, fixture, rejected_output, "-", invalid)
    assert rejected.returncode == 1
    assert [row["ok"] for row in bad_rows] == [False, False, True, False, False, False, False, False, True], bad_rows
    assert bad_rows[0]["advanced"] == 0
    assert sorted(path.name for path in rejected_output.iterdir()) == ["first.png"]
    assert not (work / "outside.png").exists()

    reused, rows = replay(program, fixture, file_output, command_file)
    assert reused.returncode != 0 and not rows
    assert (file_output / "frames/initial.png").read_bytes() == initial


def main():
    with tempfile.TemporaryDirectory(prefix="coastal-script-test-") as temporary:
        validate(Path(sys.argv[1]).resolve(), Path(temporary))
    print("Windowless fixture replay passed for file, stdin, input, captures and rejected commands.")


if __name__ == "__main__":
    main()
