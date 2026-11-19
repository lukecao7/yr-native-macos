"""Generate the self-authored host fixture with a single palette entry."""

import struct
import sys
from pathlib import Path


def main() -> None:
    target = Path(sys.argv[1])
    target.mkdir(parents=True, exist_ok=True)
    entry = b"CHFX" + bytes((207, 233, 102, 255))
    with (target / "fixture.mix").open("xb") as archive:
        archive.write(struct.pack("<HIIII", 1, len(entry), 1, 0, len(entry)))
        archive.write(entry)


if __name__ == "__main__":
    main()
