#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow"]
# ///
"""Bake edited opponent portraits back into SYNTO.PVS.

The opponent-select screen loads SYNTO.PVS (instead of SDOSEL.PVS) in
synthwave mode. It must contain the whole SDOSEL resource set, so this script
patches the seven "opp0".."opp6" shapes inside a template container and
RLE-compresses the result.

Get the template once from a dev build:

    REST2.EXE /dumpdosel          # writes DOSEL.BIN (decompressed container)

Then edit opp0.png..opp6.png and bake:

    uv run src/tools/synthwave/bake_opponents.py \\
        --template DOSEL.BIN --png-dir src/tools/synthwave -o SYNTO.PVS
"""

import argparse
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_synt as m  # noqa: E402


def _pixels(im):
    if hasattr(im, "get_flattened_data"):
        return im.get_flattened_data()
    return im.getdata()


def image_indices(im, w, h, pal):
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
    if len(px) != w * h:
        raise SystemExit(f"expected {w*h} pixels, got {len(px)}")
    return px


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--template", required=True,
                    help="DOSEL.BIN produced by REST2.EXE /dumpdosel")
    ap.add_argument("--png-dir", default=here,
                    help="directory containing opp0.png..opp6.png")
    ap.add_argument("-o", "--out", default="SYNTO.PVS",
                    help="output shape2d file (default: SYNTO.PVS)")
    ap.add_argument("--palette", default=os.path.join(here, "synthwave.pal"),
                    help="768-byte 6-bit palette used for RGB edits")
    args = ap.parse_args()

    data = bytearray(open(args.template, "rb").read())
    res = m.parse_container(data)
    # The template is a dumped runtime container: clear the flip flags so the
    # game does not unflip already-unflipped pixels a second time.
    for o, _w, _h in res.values():
        data[o + 14] = 0
        data[o + 15] = 0
    pal = m.load_palette(args.palette)

    for k in range(7):
        name = f"opp{k}"
        if name not in res:
            raise SystemExit(f"{args.template}: missing resource {name}")
        o, w, h = res[name]
        path = os.path.join(args.png_dir, name + ".png")
        im = Image.open(path)
        if im.size != (w, h):
            raise SystemExit(f"{path}: expected {w}x{h}, got {im.width}x{im.height}")
        data[o + 16:o + 16 + w * h] = bytes(image_indices(im, w, h, pal))

    blob = m.rle_encode(bytes(data))
    open(args.out, "wb").write(blob)
    print(f"wrote {args.out}: {len(blob)} bytes ({len(data)} decoded)")


if __name__ == "__main__":
    main()
