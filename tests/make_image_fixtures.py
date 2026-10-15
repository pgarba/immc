#!/usr/bin/env python3
"""Regenerate the JPEG fixtures tests/test_img.cpp reads.

They are committed rather than built by the test because a JPEG cannot be written
by hand the way a PNG or a BMP can, and CI installs no image tool - a test that
needed one would quietly skip the one format most pictures are.

Both are 16 wide by 8 tall, every pixel green except a 4x4 red block in the top
left corner. That shape answers the question the test asks: an asymmetric picture
shows which way it turned, a non-square one shows whether the size swapped with it,
and a block rather than a single dot survives the chroma subsampling a JPEG does to
its colours.
fixture_orient6.jpg carries EXIF orientation 6 (rotate 90 clockwise), so the red
block must land in the top right of an 8x16 picture. fixture_plain.jpg carries no
orientation at all and must leave the picture alone.

    python3 tests/make_image_fixtures.py
"""
import os

from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))


def base():
    im = Image.new("RGB", (16, 8), (0, 255, 0))
    for x in range(4):
        for y in range(4):
            im.putpixel((x, y), (255, 0, 0))
    return im


def main():
    plain = os.path.join(here, "fixture_plain.jpg")
    o6 = os.path.join(here, "fixture_orient6.jpg")

    base().save(plain, "JPEG", quality=95)

    im = base()
    ex = im.getexif()
    ex[274] = 6                      # 274 is 0x0112, the orientation tag
    im.save(o6, "JPEG", quality=95, exif=ex.tobytes())

    for p in (plain, o6):
        print(os.path.basename(p), os.path.getsize(p), "bytes")


if __name__ == "__main__":
    main()
