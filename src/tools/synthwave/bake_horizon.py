#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow"]
# ///
"""Bake an edited horizon panorama back into SYNT.PVS.

The horizon is one 1024x48 image split into four bottom-aligned slices
(scen 320x48, sce2 192x32, sce3 320x44, sce4 192x24) — the geometry the
skybox blitter expects. The palette resource (!pal) comes from
synthwave.pal, so editing the PNG only changes the art.

    uv run src/tools/synthwave/bake_horizon.py horizon.png -o SYNT.PVS

Then copy SYNT.PVS next to REST2.EXE (or into the web game bundle).
"""

import argparse
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_synt as m  # noqa: E402

W, H = 1024, 48
SLICES = [
    ("scen", 320, 48, 0),
    ("sce2", 192, 32, 320),
    ("sce3", 320, 44, 512),
    ("sce4", 192, 24, 832),
]


def _pixels(im):
    if hasattr(im, "get_flattened_data"):
        return im.get_flattened_data()
    return im.getdata()


def image_indices(im, pal):
    if im.mode == "P":
        px = list(_pixels(im))
    else:
        near = m.Nearest(pal)
        px = []
        cache = near.cache
        for r, g, b in _pixels(im.convert("RGB")):
            key = (r * 63 // 255, g * 63 // 255, b * 63 // 255)
            hit = cache.get(key)
            if hit is None:
                hit = near(key)
            px.append(hit)
    if len(px) != W * H:
        raise SystemExit(f"expected {W*H} pixels, got {len(px)}")
    return px


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help="1024x48 horizon PNG")
    ap.add_argument("-o", "--out", default="SYNT.PVS",
                    help="output shape2d file (default: SYNT.PVS)")
    ap.add_argument("--palette", default=os.path.join(here, "synthwave.pal"),
                    help="768-byte 6-bit palette written to !pal")
    args = ap.parse_args()

    im = Image.open(args.image)
    if im.size != (W, H):
        raise SystemExit(f"{args.image}: expected {W}x{H}, got {im.width}x{im.height}")

    pal = m.load_palette(args.palette)
    px = image_indices(im, pal)

    resources = [("!pal", m.shape_bytes(
        768, 1, [c for r, g, b in pal for c in (r, g, b)]))]
    for name, w, h, x in SLICES:
        crop = []
        for y in range(H - h, H):
            crop.extend(px[y * W + x:y * W + x + w])
        resources.append((name, m.shape_bytes(w, h, crop)))

    blob = m.rle_encode(m.build_container(resources))
    open(args.out, "wb").write(blob)
    print(f"wrote {args.out}: {len(blob)} bytes")


if __name__ == "__main__":
    main()
