#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Bake edited opponent descriptions into SYN1.RES..SYN6.RES.

In synthwave mode the opponent loader asks for "synN" instead of "oppN", so a
raw resource file SYN<n>.RES takes precedence over the game's OPP<n>.PRE. Each
file carries the same AI data (path/sped/winn and the win/lose dialogue); only
the description ("des") and the two-letter initials ("nam") are replaced.

Get the templates once from a dev build:

    REST2.EXE /dumpopp          # writes OPP1.BIN .. OPP6.BIN

Extract the current texts into an editable file:

    uv run src/tools/synthwave/bake_opponent_text.py --extract \\
        --template-dir . -o src/tools/synthwave/opponents.txt

Edit opponents.txt, then bake the .RES files:

    uv run src/tools/synthwave/bake_opponent_text.py \\
        --template-dir . --text src/tools/synthwave/opponents.txt --out-dir .
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_synt as m  # noqa: E402


def parse_resources(data):
    """Return [[name_bytes, blob], ...] from a raw resource container."""
    n = struct.unpack_from("<H", data, 4)[0]
    base = 6 + n * 8
    names = []
    offs = []
    for i in range(n):
        names.append(data[6 + i * 4:10 + i * 4])
        offs.append(struct.unpack_from("<I", data, 6 + n * 4 + i * 4)[0])
    order = sorted(range(n), key=lambda i: offs[i])
    nxt = {}
    for k, i in enumerate(order):
        nxt[i] = offs[order[k + 1]] if k + 1 < len(order) else len(data) - base
    res = []
    for i in range(n):
        off = offs[i]
        res.append([names[i], data[base + off:base + off + (nxt[i] - off)]])
    return res


def find_res(res, suffix):
    for i, (name, _blob) in enumerate(res):
        if name[1:] == suffix:
            return i
    return None


def cstr(blob):
    return blob.split(b"\0", 1)[0]


def read_text(path):
    """Parse '# oppN' sections; returns {n: (initials, [lines])}."""
    sections = {}
    cur = None
    lines = []
    initials = ""
    for raw in open(path, encoding="latin1"):
        line = raw.rstrip("\r\n")
        s = line.strip()
        if s.lower().startswith("# opp"):
            if cur is not None:
                sections[cur] = (initials, lines)
            cur = int("".join(c for c in s.split()[-1] if c.isdigit()))
            lines = []
            initials = ""
        elif cur is not None:
            if s.lower().startswith("initials:"):
                initials = s.split(":", 1)[1].strip()
            else:
                lines.append(line)
    if cur is not None:
        sections[cur] = (initials, lines)
    return sections


def extract(args):
    out = []
    for n in range(1, 7):
        path = os.path.join(args.template_dir, f"OPP{n}.BIN")
        res = parse_resources(open(path, "rb").read())
        di = find_res(res, b"des")
        ni = find_res(res, b"nam")
        if di is None or ni is None:
            raise SystemExit(f"{path}: missing des/nam resource")
        initials = cstr(res[ni][1]).decode("latin1")
        desc = cstr(res[di][1]).decode("latin1").replace("]", "\n")
        out.append(f"# opp{n}")
        out.append(f"initials: {initials}")
        out.append(desc)
    open(args.out, "w", encoding="latin1").write("\n".join(out) + "\n")
    print(f"wrote {args.out}")


def bake(args):
    sections = read_text(args.text)
    os.makedirs(args.out_dir, exist_ok=True)
    for n in range(1, 7):
        path = os.path.join(args.template_dir, f"OPP{n}.BIN")
        res = parse_resources(open(path, "rb").read())
        di = find_res(res, b"des")
        ni = find_res(res, b"nam")
        if di is None or ni is None:
            raise SystemExit(f"{path}: missing des/nam resource")
        if n not in sections:
            raise SystemExit(f"{args.text}: no '# opp{n}' section")
        initials, lines = sections[n]
        initials = initials[:2]
        res[ni][1] = initials.encode("latin1") + b"\0"
        res[di][1] = "]".join(lines).encode("latin1") + b"\0"
        container = m.build_container(
            [(name.decode("latin1"), blob) for name, blob in res])
        out = os.path.join(args.out_dir, f"SYN{n}.RES")
        open(out, "wb").write(container)
        print(f"wrote {out}: {len(container)} bytes")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--template-dir", default=".",
                    help="directory with OPP1.BIN..OPP6.BIN")
    ap.add_argument("--text", default=os.path.join(here, "opponents.txt"),
                    help="editable descriptions (default: opponents.txt)")
    ap.add_argument("--out-dir", default=".")
    ap.add_argument("--extract", action="store_true",
                    help="write --out (a text file) from the templates")
    ap.add_argument("-o", "--out", help="output for --extract")
    args = ap.parse_args()
    if args.extract:
        if not args.out:
            raise SystemExit("--extract requires -o FILE")
        extract(args)
    else:
        bake(args)


if __name__ == "__main__":
    main()
