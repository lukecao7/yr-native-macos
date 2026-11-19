"""Generate an original 128 by 128 RTS terrain and install its public rules."""

import shutil
import sys
from pathlib import Path

SIZE = 128


def basin() -> list[str]:
    rows = [["."] * SIZE for _ in range(SIZE)]
    for y in range(10, SIZE - 10):
        if not 55 <= y <= 72:
            for x in range(58, 63):
                rows[y][x] = "~"
    for y in range(35, 52):
        for x in range(39, 52):
            rows[y][x] = "/" if x in (39, 51) or y in (35, 51) else "="
    for x, y in ((50, 57), (51, 57), (52, 57), (70, 69), (70, 70), (71, 70)):
        rows[y][x] = "#"
    for x, y in ((32, 62), (33, 62), (32, 61), (87, 62), (86, 62), (87, 61)):
        rows[y][x] = "o"
    rows[64][26] = "a"
    rows[64][93] = "b"
    return ["".join(row) for row in rows]


def main() -> None:
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    with (output / "Basin.map").open("x", encoding="ascii") as stream:
        stream.write(f"COASTAL_MAP 1\n{SIZE} {SIZE} 240\n")
        stream.write("\n".join(basin()) + "\n")
    shutil.copyfile(Path(__file__).parent / "fixtures" / "coastal_rules.tsv",
                    output / "coastal_rules.tsv")


if __name__ == "__main__":
    main()
