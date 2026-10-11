#!/usr/bin/env python3
# The icons for the Windows version, which has no icon theme of its own to borrow from:
# DiskForge Live's, drawn for its dark desktop, and a light copy for Windows' usual light
# windows, with each of its colours swapped for one that stands out on white.
#   icons.py <DiskForge Live's theme folder> <out folder>
# Ends with <out>/DiskForge-Dark and <out>/DiskForge-Light; DiskForge picks one by how light
# its windows are.
import os
import shutil
import sys

# The Live palette and what it becomes on white (Windows 11's own blue, red and green where
# it has one).
LIGHT = {
    "#9ad8ff": "#1b4f8a",  # baby blue lines: dark blue
    "#00e5ff": "#0067c0",  # neon accents: Windows' blue
    "#1f6fff": "#1f6fff",  # blue holds up on white as it is
    "#ff3355": "#c42b1c",  # red
    "#2bee8a": "#0f7b0f",  # green
    "#ffb300": "#9d5d00",  # amber
    "#060a16": "#ffffff",  # the dark desktop showing through: white here
}


def copy_theme(source, out, name, comment, colours):
    shutil.rmtree(out, ignore_errors=True)
    for folder, _, files in os.walk(source):
        target = os.path.join(out, os.path.relpath(folder, source))
        os.makedirs(target, exist_ok=True)
        for f in files:
            if not f.endswith(".svg"):
                continue
            with open(os.path.join(folder, f), encoding="utf-8") as svg:
                text = svg.read()
            for old, new in colours.items():
                text = text.replace(old, new).replace(old.upper(), new)
            with open(os.path.join(target, f), "w", encoding="utf-8") as svg:
                svg.write(text)
    # The same folders as Live's, without the themes it inherits from (Windows has none).
    with open(os.path.join(source, "index.theme"), encoding="utf-8") as theme:
        lines = theme.read().splitlines()
    with open(os.path.join(out, "index.theme"), "w", encoding="utf-8") as theme:
        for line in lines:
            if line.startswith("Name="):
                line = "Name=" + name
            elif line.startswith("Comment="):
                line = "Comment=" + comment
            elif line.startswith("Inherits="):
                line = "Inherits=hicolor"
            theme.write(line + "\n")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__ or "icons.py <theme folder> <out folder>")
    source, out = sys.argv[1], sys.argv[2]
    copy_theme(source, os.path.join(out, "DiskForge-Dark"), "DiskForge Dark", "DiskForge Live's own icons", {})
    copy_theme(source, os.path.join(out, "DiskForge-Light"), "DiskForge Light", "DiskForge Live's icons, for light windows", LIGHT)


if __name__ == "__main__":
    main()
