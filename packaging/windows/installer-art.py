#!/usr/bin/env python3
# The pictures in DiskAnvil's Windows installer, drawn from its own logo at build time: the
# side panel of the first and last pages (493x312) and the strip along the top of the others
# (493x58), as the 24-bit BMPs Windows Installer wants.
#   installer-art.py <logo.svg> <out folder>
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

NAVY = (13, 20, 38)
WHITE = (255, 255, 255)
BLUE = (154, 216, 255)


def logo(svg, size):
    png = subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), svg], capture_output=True, check=True).stdout
    from io import BytesIO
    return Image.open(BytesIO(png)).convert("RGBA")


def font(size):
    for path in ("/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def main():
    if len(sys.argv) != 3:
        sys.exit("installer-art.py <logo.svg> <out folder>")
    svg, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)

    # The first and last pages: a dark panel on the left (164 wide, where the dialogs leave room
    # for it) with the logo and the name, white where the text goes.
    dialog = Image.new("RGB", (493, 312), WHITE)
    draw = ImageDraw.Draw(dialog)
    draw.rectangle((0, 0, 163, 311), fill=NAVY)
    mark = logo(svg, 96)
    dialog.paste(mark, ((164 - 96) // 2, 70), mark)
    name = font(20)
    width = draw.textlength("DiskAnvil", font=name)
    draw.text(((164 - width) / 2, 180), "DiskAnvil", font=name, fill=WHITE)
    draw.line((40, 214, 124, 214), fill=BLUE, width=2)
    dialog.save(os.path.join(out, "dialog.bmp"))

    # The strip along the top: white, with the logo at the right.
    banner = Image.new("RGB", (493, 58), WHITE)
    small = logo(svg, 44)
    banner.paste(small, (493 - 44 - 8, 7), small)
    banner.save(os.path.join(out, "banner.bmp"))


if __name__ == "__main__":
    main()
