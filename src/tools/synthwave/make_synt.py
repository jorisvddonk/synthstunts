#!/usr/bin/env python3
"""Build SYNT.PVS, the /synthwave asset for restunts2.

SYNT.PVS is a normal shape2d container (the format the game's
file_load_shape2d() understands) holding:

  !pal     768x1, 768 bytes of 6-bit RGB (the replacement DAC palette)
  scen     320xh skyline slice
  sce2     192xh skyline slice
  sce3     320xh skyline slice
  sce4     192xh skyline slice

The four slices are consecutive pieces of a 1024-px panorama (that is the
wrap period and the relative x offsets the skybox blitter uses: 0x140/0x200/
0x340). Each slice is bottom-aligned on the horizon, so its height is how far
it reaches into the sky.

The file is compressed with the game's single-pass RLE (type 1). Header:

  [0]      0x01                     RLE, single pass
  [1..3]   decompressed length      (24-bit LE)
  [4..7]   token stream length      (32-bit LE)
  [8]      0x83                     bit7 = skip seq pass, 3 escape codes
  [9..11]  escape bytes
  [12..]   tokens

Tokens: literal byte (not an escape), or
  esc1 count value        -> count copies (count 1..255)
  esc2 value              -> one copy
  esc3 lo hi value        -> count copies (16-bit)

Shapes use fliptype 0 / unk6 0, which file_unflip_shape2d() skips, so pixels
stay raw row-major.

Usage:
  make_synt.py palette  --palette PALETTE.BIN --out-dir OUT [--hue ..]
  make_synt.py preview  --palette PALETTE.BIN --shot SHOT.png --out OUT.png
  make_synt.py build    --palette PALETTE.BIN --out SYNT.PVS [--preview-dir D]
  make_synt.py check    --file SYNT.PVS

Requires Pillow for the preview/build image work.
"""

import argparse
import colorsys
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    Image = None


# --------------------------------------------------------------------------
# palette
# --------------------------------------------------------------------------
# Flat-fill and accent indices the game itself uses.
IDX_SKY = 0x74     # material_color_list[0x22], sky fill above the strips
IDX_GROUND = 0x6C  # material_color_list[0x20], ground fill
IDX_WATER = 0xC8   # material_color_list[0xC8], water fill

# Slice geometry: (name, width, height). Heights are how far each slice
# reaches above the horizon. Total = 1024 px wide.
SLICES = [
    ("scen", 320, 48),
    ("sce2", 192, 32),
    ("sce3", 320, 44),
    ("sce4", 192, 24),
]
PANORAMA_W = sum(w for _, w, _ in SLICES)
PANORAMA_H = max(h for _, _, h in SLICES)


def load_palette(path):
    data = open(path, "rb").read()
    if len(data) < 768:
        raise SystemExit(f"{path}: expected 768 bytes, got {len(data)}")
    return [tuple(data[i * 3:i * 3 + 3]) for i in range(256)]


def _clamp6(v):
    return max(0, min(63, int(round(v))))


def transform_palette(pal, hue=0.62, sat=1.5, sat_add=0.12, val=0.92,
                      tint=0.30, sky=(8, 2, 26), ground=(18, 3, 28),
                      water=(46, 6, 34)):
    """Shift the whole palette toward neon, then force the flat-fill indices.

    Vivid colours get a hue rotation and a saturation boost (cars, scenery).
    Low-saturation colours (asphalt, UI greys) are mapped onto a blue/violet
    ramp so the road becomes a synthwave highway instead of staying brown.
    sky/ground/water are the three indices the skybox code reads, so forcing
    them guarantees the flat regions match the new panorama.
    """
    out = []
    for rgb in pal:
        r, g, b = (v / 63.0 for v in rgb)
        h, s, v = colorsys.rgb_to_hsv(r, g, b)
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        if s < 0.35:
            # Greyish: blue/violet tint, strongest in the mid tones so white
            # text stays white and black stays black.
            h = 0.72
            s = 0.30 * (1.0 - lum) + 0.55 * lum * (1.0 - lum) * 2.0
            v = min(1.0, v * 1.18 + 0.06)
        else:
            h = (h + hue) % 1.0
            s = min(1.0, s * sat + sat_add)
            v = v * val
        r, g, b = colorsys.hsv_to_rgb(h, s, v)
        out.append((_clamp6(r * 63), _clamp6(g * 63), _clamp6(b * 63)))
    out[IDX_SKY] = tuple(sky)
    out[IDX_GROUND] = tuple(ground)
    out[IDX_WATER] = tuple(water)
    return out


def pal8(pal):
    """6-bit palette -> flat 8-bit list for Pillow."""
    out = []
    for r, g, b in pal:
        out += [r * 255 // 63, g * 255 // 63, b * 255 // 63]
    return out


def swatch_png(pal, path, cell=20):
    im = Image.new("RGB", (16 * cell, 16 * cell))
    d = ImageDraw.Draw(im)
    for i, (r, g, b) in enumerate(pal):
        x = (i % 16) * cell
        y = (i // 16) * cell
        d.rectangle([x, y, x + cell - 1, y + cell - 1],
                    fill=(r * 255 // 63, g * 255 // 63, b * 255 // 63))
    im.save(path)


# --------------------------------------------------------------------------
# panorama
# --------------------------------------------------------------------------
def _lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def _gradient(d, stops):
    """d = distance above the horizon (0 = horizon), stops sorted by d."""
    if d <= stops[0][0]:
        return stops[0][1]
    for (d0, c0), (d1, c1) in zip(stops, stops[1:]):
        if d <= d1:
            return _lerp(c0, c1, (d - d0) / max(1, d1 - d0))
    return stops[-1][1]


class Nearest:
    """6-bit RGB -> palette index, cached."""

    def __init__(self, pal):
        self.pal = pal
        self.cache = {}

    def __call__(self, rgb):
        hit = self.cache.get(rgb)
        if hit is not None:
            return hit
        best = 0
        bestd = 1 << 30
        r, g, b = rgb
        for i, (pr, pg, pb) in enumerate(self.pal):
            d = (pr - r) ** 2 + (pg - g) ** 2 + (pb - b) ** 2
            if d < bestd:
                bestd, best = d, i
                if d == 0:
                    break
        self.cache[rgb] = best
        return best


def make_panorama_indexed(pal):
    """1024 x H index buffer: sunset gradient, striped sun, neon ridges."""
    W, H = PANORAMA_W, PANORAMA_H
    near = Nearest(pal)
    # d is measured up from the horizon, so slices of different heights line
    # up when they are bottom-aligned.
    sky_stops = [
        (0, (56, 10, 42)),   # horizon: magenta
        (10, (48, 8, 46)),   # purple
        (22, (32, 6, 46)),   # indigo
        (34, (16, 4, 38)),   # deep purple
        (48, (8, 2, 26)),    # matches the forced sky fill
    ]
    ridge_back = (14, 2, 34)
    ridge_front = (7, 1, 20)
    neon = (58, 10, 44)
    sun_top = (58, 46, 12)
    sun_bot = (60, 14, 34)

    rows = []
    for y in range(H):
        d = H - 1 - y  # 0 at the bottom (horizon)
        base = near(_gradient(d, sky_stops))
        rows.append([base] * W)

    def ridge(x, amp, base_d, phase, freq):
        import math
        return base_d + amp * (0.55 + 0.45 * math.sin(x * freq + phase)
                               + 0.25 * math.sin(x * freq * 2.7 + phase * 1.7))

    for x in range(W):
        top = ridge(x, 13, 6, 0.7, 0.012)
        ci = near(ridge_back)
        for y in range(H):
            if H - 1 - y <= top:
                rows[y][x] = ci
    for x in range(W):
        top = ridge(x, 8, 2, 2.2, 0.021)
        ci = near(ridge_front)
        ne = near(neon)
        for y in range(H):
            d = H - 1 - y
            if d <= top:
                rows[y][x] = ci
                if abs(d - top) < 1.1:
                    rows[y][x] = ne

    # Sun: disc with horizontal cut-outs, sitting above the horizon.
    cx, cd, r = 512, 19, 12
    for y in range(H):
        d = H - 1 - y
        for x in range(cx - r, cx + r + 1):
            dx = x - cx
            dy = d - cd
            if dx * dx + dy * dy <= r * r:
                if (d - (cd - r)) % 3 == 0 and d < cd + 3:
                    continue
                t = (d - (cd - r)) / (2 * r)
                rows[y][x] = near(_lerp(sun_bot, sun_top,
                                        max(0.0, min(1.0, t))))

    # A thin neon horizon line so the ground reads as a separate plane.
    line = near((56, 8, 40))
    for x in range(W):
        rows[H - 1][x] = line
    return rows


def slice_panorama(rows):
    """Crop the bottom h rows of each slice, preserving x ranges."""
    out = []
    x = 0
    for name, w, h in SLICES:
        crop = [row[x:x + w] for row in rows[PANORAMA_H - h:]]
        out.append((name, w, h, crop))
        x += w
    return out


def rows_to_rgb(rows, pal):
    im = Image.new("RGB", (len(rows[0]), len(rows)))
    px = im.load()
    for y, row in enumerate(rows):
        for x, i in enumerate(row):
            r, g, b = pal[i]
            px[x, y] = (r * 255 // 63, g * 255 // 63, b * 255 // 63)
    return im


def rows_to_flat(rows):
    return [i for row in rows for i in row]


# --------------------------------------------------------------------------
# shape2d container
# --------------------------------------------------------------------------
def shape_bytes(w, h, pixels):
    if len(pixels) != w * h:
        raise ValueError(f"shape {w}x{h}: got {len(pixels)} pixels")
    hdr = struct.pack("<HHHHHHBBBB", w, h, 0, 0, 0, 0, 0, 0, 0, 0)
    return hdr + bytes(pixels)


def build_container(resources):
    """resources: [(4-char name, shape bytes)] -> container bytes."""
    n = len(resources)
    out = bytearray(4)
    out += struct.pack("<H", n)
    for name, _ in resources:
        raw = name.encode("ascii")[:4].ljust(4, b"\0")
        out += raw
    offsets = []
    data = bytearray()
    for _, shape in resources:
        offsets.append(len(data))
        data += shape
    for off in offsets:
        out += struct.pack("<I", off)
    out += data
    return bytes(out)


# --------------------------------------------------------------------------
# RLE
# --------------------------------------------------------------------------
def rle_encode(data, esc=(0xFE, 0xFF, 0xFD)):
    e1, e2, e3 = esc
    escset = set(esc)
    toks = bytearray()
    i = 0
    n = len(data)
    while i < n:
        v = data[i]
        run = 1
        while i + run < n and data[i + run] == v and run < 0xFFFF:
            run += 1
        if run >= 4:
            if run <= 255:
                toks += bytes((e1, run, v))
            else:
                toks += bytes((e3, run & 0xFF, (run >> 8) & 0xFF, v))
        else:
            for _ in range(run):
                if v in escset:
                    toks += bytes((e2, v))
                else:
                    toks.append(v)
        i += run
    declen = len(data)
    enclen = len(toks)
    hdr = bytes((0x01, declen & 0xFF, (declen >> 8) & 0xFF,
                 (declen >> 16) & 0xFF,
                 enclen & 0xFF, (enclen >> 8) & 0xFF,
                 (enclen >> 16) & 0xFF, (enclen >> 24) & 0xFF,
                 0x83, e1, e2, e3))
    return hdr + toks


def rle_decode(blob):
    if blob[0] != 0x01:
        raise ValueError("not single-pass RLE")
    declen = blob[1] | (blob[2] << 8) | (blob[3] << 16)
    enclen = int.from_bytes(blob[4:8], "little")
    esclen = blob[8] & 0x7F
    esc = list(blob[9:9 + esclen])
    toks = blob[9 + esclen:9 + esclen + enclen]
    out = bytearray()
    i = 0
    while i < len(toks):
        b = toks[i]
        if b not in esc:
            out.append(b)
            i += 1
            continue
        idx = esc.index(b) + 1
        if idx == 1:
            out += bytes((toks[i + 2],)) * toks[i + 1]
            i += 3
        elif idx == 2:
            out.append(toks[i + 1])
            i += 2
        elif idx == 3:
            cnt = toks[i + 1] | (toks[i + 2] << 8)
            out += bytes((toks[i + 3],)) * cnt
            i += 4
        else:
            raise ValueError(f"bad escape index {idx}")
    if len(out) != declen:
        raise ValueError(f"declen mismatch: {len(out)} != {declen}")
    return bytes(out)


# --------------------------------------------------------------------------
# preview
# --------------------------------------------------------------------------
def quantize_to_pal(rgb_im, pal):
    p = Image.new("P", (1, 1))
    p.putpalette(pal8(pal))
    return rgb_im.quantize(palette=p, dither=Image.Dither.NONE)


def recolor_shot(shot_path, orig_pal, new_pal, out_path, width=320,
                 height=200):
    shot = Image.open(shot_path).convert("RGB")
    shot = shot.resize((width, height), Image.LANCZOS)
    idx = quantize_to_pal(shot, orig_pal)
    idx.putpalette(pal8(new_pal))
    idx.convert("RGB").resize((width * 2, height * 2),
                              Image.NEAREST).save(out_path)


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def cmd_palette(args):
    pal = load_palette(args.palette)
    new = transform_palette(pal, hue=args.hue, sat=args.sat, val=args.val,
                            tint=args.tint)
    os.makedirs(args.out_dir, exist_ok=True)
    swatch_png(pal, os.path.join(args.out_dir, "palette-original.png"))
    swatch_png(new, os.path.join(args.out_dir, "palette-synthwave.png"))
    print("wrote", args.out_dir)


def cmd_preview(args):
    pal = load_palette(args.palette)
    new = transform_palette(pal, hue=args.hue, sat=args.sat, val=args.val,
                            tint=args.tint)
    recolor_shot(args.shot, pal, new, args.out)
    print("wrote", args.out)


def cmd_build(args):
    pal = load_palette(args.palette)
    new = transform_palette(pal, hue=args.hue, sat=args.sat, val=args.val,
                            tint=args.tint)

    rows = make_panorama_indexed(new)
    resources = [("!pal", shape_bytes(768, 1,
                                      [c for rgb in new for c in rgb]))]
    for name, w, h, crop in slice_panorama(rows):
        # the top row must equal the flat sky fill, or a seam shows above a
        # shorter slice
        for x in range(w):
            crop[0][x] = IDX_SKY
        resources.append((name, shape_bytes(w, h, rows_to_flat(crop))))
        if args.preview_dir:
            os.makedirs(args.preview_dir, exist_ok=True)
            rows_to_rgb(crop, new).save(
                os.path.join(args.preview_dir, f"{name}.png"))

    container = build_container(resources)
    blob = container if args.raw else rle_encode(container)
    open(args.out, "wb").write(blob)
    if args.preview_dir:
        os.makedirs(args.preview_dir, exist_ok=True)
        rows_to_rgb(rows, new).save(
            os.path.join(args.preview_dir, "panorama.png"))
        swatch_png(new, os.path.join(args.preview_dir, "palette.png"))
        # decoded round-trip check
        if not args.raw:
            dec = rle_decode(blob)
            if dec != container:
                raise SystemExit("RLE round-trip mismatch")
        print(f"wrote {args.out}: {len(blob)} bytes "
              f"({len(container)} decoded)")
    else:
        print(f"wrote {args.out}: {len(blob)} bytes")


def cmd_check(args):
    blob = open(args.file, "rb").read()
    dec = rle_decode(blob)
    n = struct.unpack_from("<H", dec, 4)[0]
    print(f"ok: {len(blob)} bytes, {len(dec)} decoded, {n} resources")
    names = [dec[6 + i * 4:10 + i * 4].decode("ascii", "replace")
             for i in range(n)]
    print("resources:", names)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--palette", required=True)
        p.add_argument("--hue", type=float, default=0.58)
        p.add_argument("--sat", type=float, default=1.45)
        p.add_argument("--val", type=float, default=0.92)
        p.add_argument("--tint", type=float, default=0.30)

    p = sub.add_parser("palette")
    common(p)
    p.add_argument("--out-dir", required=True)
    p.set_defaults(func=cmd_palette)

    p = sub.add_parser("preview")
    common(p)
    p.add_argument("--shot", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_preview)

    p = sub.add_parser("build")
    common(p)
    p.add_argument("--out", required=True)
    p.add_argument("--preview-dir")
    p.add_argument("--raw", action="store_true",
                   help="write the uncompressed container (.DAT)")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("check")
    p.add_argument("--file", required=True)
    p.set_defaults(func=cmd_check)

    args = ap.parse_args()
    if Image is None and args.cmd in ("palette", "preview", "build"):
        raise SystemExit("Pillow is required for this command")
    args.func(args)


if __name__ == "__main__":
    main()
