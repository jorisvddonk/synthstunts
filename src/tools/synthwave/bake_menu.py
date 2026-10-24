#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow"]
# ///
"""Bake an edited 320x200 menu image back into SYNTM.PVS.

The game loads SYNTM.PVS as a normal shape2d file containing one shape named
"scrn" (see run_menu_asm_ -> file_load_resource("syntm", 2)). This script takes
a PNG (indexed or RGB), rebuilds that shape with the original 16-byte header,
and writes the RLE-compressed container.

Run with uv (installs Pillow automatically):

    uv run src/tools/synthwave/bake_menu.py menu.png -o SYNTM.PVS

Then copy SYNTM.PVS next to REST2.EXE (or into the web game bundle).
"""

import argparse
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_synt as m  # noqa: E402

W, H = 320, 200
# Original SDMSEL.PVS "scrn" header: w, h, unk1, unk2, posx, posy, u3..u6.
# Only the pixel data changes; keeping the header identical keeps the blit
# position/size exactly as the game expects.
HEADER = struct.pack("<HHHHHHBBBB", W, H, 188, 132, 0, 0, 0x01, 0x02, 0x04, 0x08)


def _pixels(im):
    # Pillow 14 renames getdata(); support both.
    if hasattr(im, "get_flattened_data"):
        return im.get_flattened_data()
    return im.getdata()


def image_indices(im, pal):
    if im.mode == "P":
        return list(_pixels(im))
    if im.mode not in ("RGB", "RGBA"):
        im = im.convert("RGB")
    near = m.Nearest(pal)
    out = []
    cache = near.cache
    for r, g, b in _pixels(im.convert("RGB")):
        key = (r * 63 // 255, g * 63 // 255, b * 63 // 255)
        hit = cache.get(key)
        if hit is None:
            hit = near(key)
        out.append(hit)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help="320x200 PNG to bake")
    ap.add_argument("-o", "--out", default="SYNTM.PVS",
                    help="output shape2d file (default: SYNTM.PVS)")
    ap.add_argument("--palette", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "synthwave.pal"),
        help="768-byte 6-bit palette used when the PNG is RGB")
    args = ap.parse_args()

    im = Image.open(args.image)
    if im.size != (W, H):
        raise SystemExit(f"{args.image}: expected {W}x{H}, got {im.width}x{im.height}")

    pal = m.load_palette(args.palette)
    px = image_indices(im, pal)
    if len(px) != W * H:
        raise SystemExit(f"{args.image}: expected {W*H} pixels, got {len(px)}")

    shape = HEADER + bytes(px)
    container = m.build_container([("scrn", shape)])
    blob = m.rle_encode(container)
    open(args.out, "wb").write(blob)
    print(f"wrote {args.out}: {len(blob)} bytes ({len(container)} decoded)")


if __name__ == "__main__":
    main()
