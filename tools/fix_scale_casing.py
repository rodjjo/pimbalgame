#!/usr/bin/env python3
"""Fix SFML sprite scale() -> setScale() casing.

The SFML Sprite transform methods are camelCase (setScale, setOrigin, ...).
When these source files were typed, the scale() call sometimes lost its "set"
prefix, producing a non-existent s.scale(...). This script finds every
".scale(" occurrence and rewrites it as ".set" + "Scale(" == ".setScale(".
"""
import pathlib
import re
import sys

REPLACEMENT = ".set" + "Scale("  # build ".setScale(" without typing it outright

def main(paths):
    changed = 0
    for path in paths:
        text = pathlib.Path(path).read_text(encoding="utf-8")
        new_text, n = re.subn(r"\.scale\(", REPLACEMENT, text)
        if n:
            pathlib.Path(path).write_text(new_text, encoding="utf-8")
            print(f"{path}: replaced {n} occurrence(s)")
            changed += n
    print(f"total replacements: {changed}")

if __name__ == "__main__":
    main(sys.argv[1:] or ["src/pimbalgame/World.cpp"])
