#!/usr/bin/env python3
# The files the lost-files test images are made with, always the same bytes: pictures of
# random dots (they don't pack small, so they span several clusters), a small PDF and text.
#   files.py <folder>
import random
import sys

from PIL import Image

out = sys.argv[1]


def dots(path, w, h, seed, fmt):
    rnd = random.Random(seed)
    img = Image.new("RGB", (w, h))
    img.putdata([(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(w * h)])
    img.save(path, fmt, **({"quality": 90} if fmt == "JPEG" else {}))


dots(f"{out}/photo.jpg", 64, 48, 1, "JPEG")
dots(f"{out}/holiday.jpg", 96, 64, 2, "JPEG")
dots(f"{out}/pic1.png", 48, 48, 3, "PNG")
dots(f"{out}/pic2.jpg", 80, 60, 4, "JPEG")
dots(f"{out}/scan.png", 40, 40, 5, "PNG")
dots(f"{out}/kitten.jpg", 72, 72, 6, "JPEG")
with open(f"{out}/letter.pdf", "w") as f:
    f.write("\n".join(["%PDF-1.4", "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj",
                       "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj",
                       "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >> endobj",
                       "trailer << /Root 1 0 R >>", "%%EOF"]) + "\n")
with open(f"{out}/notes.txt", "w") as f:
    f.write("Things to remember.\nThe stick is in the drawer.\n")
with open(f"{out}/readme.txt", "w") as f:
    f.write("An old-style 8.3 name: FAT keeps no long name for it.\n")
