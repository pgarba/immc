#!/usr/bin/env python3
"""Draw assets/immc.ico, the Windows icon, from the design in assets/immc.svg.

    python scripts/make-icon.py            # needs Pillow

The .ico is committed, so a build never runs this; it is here so the icon can be
made again when the design changes. The drawing below is immc.svg's, in its own
128-unit coordinates, and is kept in step with it by hand.

One picture does not scale down to 16 pixels: the wordmark becomes a smudge and
five rows of 6 units each become a grey blur. So the sizes Windows shows in a
title bar, a taskbar and a details list (16 to 32) get the same two panes with
fewer, thicker rows and no wordmark; 40 and 48 get the SVG without its wordmark,
and 64 and up get it whole. Every size is
drawn at 8 times its resolution and scaled down, which is what keeps the rounded
corners smooth.
"""

import io
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "immc.ico")

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SMALL = 32        # this size and below get the simplified drawing
WORDMARK = 64     # and below this one the wordmark is too small to read
SUPER = 8

BG = "#1a1d21"
PANEL = "#23272e"
ACTIVE = "#3373cc"
INACTIVE_HEAD = "#2b3038"
ROW_HI = "#4d8fd6"
ROW = "#8a93a0"
TEXT = "#e8eef6"

# Monospace bold, as the SVG asks for; the first one this machine has.
FONTS = [
    "C:/Windows/Fonts/consolab.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
    "/System/Library/Fonts/Menlo.ttc",
]


def full(d, rect):
    """immc.svg, element for element."""
    rect(0, 0, 128, 128, 14, BG)
    rect(10, 14, 48, 100, 6, PANEL)
    rect(10, 14, 48, 16, 6, ACTIVE)
    rect(70, 14, 48, 100, 6, PANEL)
    rect(70, 14, 48, 16, 6, INACTIVE_HEAD)
    for y, w in ((36, 30), (48, 36), (60, 26)):
        rect(16, y, w, 6, 2, ROW_HI)
    for y, w in ((72, 34), (84, 28)):
        rect(16, y, w, 6, 2, ROW)
    for y, w in ((36, 32), (48, 26), (60, 36), (72, 30), (84, 24)):
        rect(76, y, w, 6, 2, ROW)
    rect(14, 96, 40, 8, 2, ACTIVE)


def small(d, rect):
    """The same two panes with what survives at 16 pixels: one row is 8 units."""
    rect(0, 0, 128, 128, 16, BG)
    rect(8, 10, 52, 108, 8, PANEL)
    rect(8, 10, 52, 22, 8, ACTIVE)
    rect(68, 10, 52, 108, 8, PANEL)
    rect(68, 10, 52, 22, 8, INACTIVE_HEAD)
    rect(16, 44, 34, 12, 3, ROW_HI)
    rect(16, 64, 28, 12, 3, ROW)
    rect(76, 44, 32, 12, 3, ROW)
    rect(76, 64, 26, 12, 3, ROW)
    rect(76, 84, 34, 12, 3, ROW)
    rect(12, 86, 44, 16, 3, ACTIVE)


def wordmark(img, scale):
    path = next((f for f in FONTS if os.path.exists(f)), None)
    if not path:
        sys.exit("no monospace bold font found for the wordmark")
    font = ImageFont.truetype(path, round(14 * scale))
    d = ImageDraw.Draw(img)
    # The SVG's text sits on a baseline at y=122, centred on x=64.
    d.text((64 * scale, 122 * scale), "immc", font=font, fill=TEXT, anchor="ms")


def draw(size):
    px = size * SUPER
    scale = px / 128
    img = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def rect(x, y, w, h, r, fill):
        d.rounded_rectangle([x * scale, y * scale, (x + w) * scale - 1, (y + h) * scale - 1],
                            radius=r * scale, fill=fill)

    if size <= SMALL:
        small(d, rect)
    else:
        full(d, rect)
        if size >= WORDMARK:
            wordmark(img, scale)
    return img.resize((size, size), Image.LANCZOS)


def write_ico(path, images):
    """An .ico of PNG entries, one per size, which Windows reads from Vista on."""
    blobs = []
    for im in images:
        b = io.BytesIO()
        im.save(b, "PNG", optimize=True)
        blobs.append(b.getvalue())
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for im, blob in zip(images, blobs):
        w = im.width if im.width < 256 else 0     # 0 means 256
        out += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(out + b"".join(blobs))


def main():
    images = [draw(s) for s in SIZES]
    write_ico(OUT, images)
    if "--preview" in sys.argv:
        # Every size side by side, at its real pixel size, for a look before committing.
        sheet = Image.new("RGBA", (sum(SIZES) + 10 * len(SIZES), 256), (240, 240, 240, 255))
        x = 0
        for im in images:
            sheet.paste(im, (x, 256 - im.height), im)
            x += im.width + 10
        sheet.save(os.path.join(HERE, "..", "assets", "immc-ico-preview.png"))
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
