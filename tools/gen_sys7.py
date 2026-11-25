#!/usr/bin/env python3
"""Build the System 7 theme's fonts and icons: tools/sys7/*.txt -> src/fonts/sys7.c.

    python3 tools/gen_sys7.py                 write src/fonts/sys7.c
    python3 tools/gen_sys7.py --sheet a.png   also a specimen sheet to look at

The art is one bit, drawn on a small grid (a 12 px bold face, a 9 px small
face, 16 px icons) and shown on the panel at whole multiples: the panel is
1872 x 1404, three of its pixels to one of a 624 x 468 screen. Each font the
standard theme has (include/bfont.h) gets a twin here with the same line
height and baseline, so a page laid out for one fits the other; the glyph
bitmaps are shared between the sizes and scaled as they are drawn
(bfont_t.scale). Needs nothing but Python 3.
"""
import os, re, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ART = os.path.join(HERE, "sys7")
NAMES = {"space": " ", "deg": "\x7f", "dot": "\x80"}


def read_art(path):
    """-> {name: (top, adv or None, [rows]) or ('<', other)} in file order."""
    out, cur = {}, None
    for raw in open(path, encoding="utf-8"):
        line = raw.rstrip("\n")
        if line.startswith("= "):
            p = line.split()
            if len(p) == 4 and p[2] == "<":
                out[p[1]] = ("<", p[3]); cur = None
                continue
            cur = [int(p[2]) if len(p) > 2 else 0, int(p[3]) if len(p) > 3 else None, []]
            out[p[1]] = cur
        elif cur is not None and line and set(line) <= set("#."):
            cur[2].append(line)
        elif line.strip() and not line.startswith("#"):
            sys.exit(f"{path}: cannot read {line!r}")
    for k, v in list(out.items()):
        if v[0] == "<":
            top, adv, rows = out[v[1]]
            out[k] = [top, adv, [r[::-1] for r in rows]]
    for k, (top, adv, rows) in out.items():
        w = max((len(r) for r in rows), default=0)
        out[k] = (top, adv, [r.ljust(w, ".") for r in rows])
    return out


def scale2x(rows):
    """Scale2x (EPX): double a one-bit picture, keeping its diagonals smooth."""
    h, w = len(rows), len(rows[0])
    px = lambda x, y: rows[y][x] if 0 <= x < w and 0 <= y < h else "."
    out = []
    for y in range(h):
        r0, r1 = [], []
        for x in range(w):
            p, a, b, c, d = px(x, y), px(x, y - 1), px(x + 1, y), px(x - 1, y), px(x, y + 1)
            r0 += [a if c == a and c != d and a != b else p, b if a == b and a != c and b != d else p]
            r1 += [c if d == c and d != b and c != a else p, d if b == d and b != a and d != c else p]
        out += ["".join(r0), "".join(r1)]
    return out


class Bits:
    """One shared bitmap table: rows packed 2 px/byte, 0 or 15 (as bfont.h)."""
    def __init__(self, name):
        self.name, self.data, self.at = name, bytearray(), {}

    def add(self, key, rows):
        self.at[key] = len(self.data)
        for r in rows:
            for x in range(0, len(r), 2):
                a = 15 if r[x] == "#" else 0
                b = 15 if x + 1 < len(r) and r[x + 1] == "#" else 0
                self.data.append((a << 4) | b)


def face(path, base_row):
    art = read_art(path)
    bits = Bits("s7_" + os.path.basename(path).split(".")[0])
    glyphs = {}
    for name, (top, adv, rows) in art.items():
        ch = NAMES.get(name, name)
        if len(ch) != 1:
            sys.exit(f"{path}: {name!r} is not one character")
        w = len(rows[0]) if rows else 0
        bits.add(ord(ch), rows)
        glyphs[ord(ch)] = (top, adv if adv is not None else w + 1, w, len(rows))
    missing = [chr(c) for c in list(range(32, 127)) + [0x7F, 0x80] if c not in glyphs]
    if missing:
        sys.exit(f"{path}: no glyph for {' '.join(missing)}")
    return {"bits": bits, "glyphs": glyphs, "base": base_row}


def icon_set(path, header):
    art = read_art(path)
    order = re.findall(r"#define ICON_(\w+)\s+\"\\x([0-9a-f]{2})\"", open(header).read())
    small, big, codes = Bits("s7_icons16"), Bits("s7_icons32"), []
    for name, hx in order:
        key = name.lower()
        if key not in art:
            sys.exit(f"{path}: no icon for ICON_{name}")
        rows = art[key][2]
        if len(rows) != 16 or len(rows[0]) != 16:
            sys.exit(f"{path}: {key} is {len(rows[0])} x {len(rows)}, not 16 x 16")
        code = int(hx, 16)
        small.add(code, rows); big.add(code, scale2x(rows)); codes.append(code)
    extra = set(art) - {n.lower() for n, _ in order}
    if extra:
        sys.exit(f"{path}: not in icons.h: {' '.join(sorted(extra))}")
    return small, big, codes


FACES = ["happy", "sad", "asleep", "bomb"]      # S7_HAPPY ... in include/ui_internal.h: codes 0xC0 up
FACE0 = 0xC0


def face_set(path, big):
    """The faces, drawn at 32 x 32 as they are: into the doubled icons' table."""
    art = read_art(path)
    if list(art) != FACES:
        sys.exit(f"{path}: wants {' '.join(FACES)}, in that order")
    for i, name in enumerate(FACES):
        rows = art[name][2]
        if len(rows) != 32 or len(rows[0]) != 32:
            sys.exit(f"{path}: {name} is {len(rows[0])} x {len(rows)}, not 32 x 32")
        big.add(FACE0 + i, rows)
    return [FACE0 + i for i in range(len(FACES))]


# name, face, scale, and the line height and ascent of the standard font it stands in for
TEXT = [
    ("s7_30",       "small", 3,  37,  29),   # ui30
    ("s7_34",       "small", 3,  42,  33),   # ui34
    ("s7_46",       "bold",  3,  56,  44),   # ui46
    ("s7_60",       "bold",  5,  72,  57),   # ui60
    ("s7_72",       "bold",  6,  87,  69),   # ui72
    ("s7_96",       "bold",  7, 116,  92),   # ui96
    ("s7_num180",   "bold", 15, 216, 171),   # num180
    ("s7_num270",   "bold", 21, 325, 257),   # num270
    ("s7_clock214", "bold", 18, 258, 204),   # clock214
]
# name, cell (16 or the doubled 32), scale
ICONS = [("s7_icon34", 16, 2), ("s7_icon40", 16, 3), ("s7_icon46", 16, 3), ("s7_icon170", 32, 5)]


def build():
    faces = {"bold": face(os.path.join(ART, "bold12.txt"), 8), "small": face(os.path.join(ART, "small9.txt"), 6)}
    i16, i32, codes = icon_set(os.path.join(ART, "icons16.txt"), os.path.join(ROOT, "include", "icons.h"))
    faces_ = face_set(os.path.join(ART, "faces32.txt"), i32)
    tables, fonts = [faces["bold"]["bits"], faces["small"]["bits"], i16, i32], []
    for name, fname, s, line_h, ascent in TEXT:
        f, rows = faces[fname], []
        for code in sorted(f["glyphs"]):
            top, adv, w, h = f["glyphs"][code]
            yoff = ascent - (f["base"] + 1 - top) * s
            if h and (yoff < 0 or yoff + h * s > line_h) and (chr(code).isalnum() or chr(code) in ":.%-\x7f"):
                print(f"note: {name} clips {chr(code)!r} ({yoff}..{yoff + h * s} of {line_h})")
            rows.append((code, adv * s, w, h, 0, yoff, f["bits"].at[code]))
        fonts.append((name, rows, line_h, ascent, f["bits"].name, s))
    for name, cell, s in ICONS:
        bits = i16 if cell == 16 else i32
        rows = [(code, cell * s, cell, cell, 0, 0, bits.at[code]) for code in codes + (faces_ if cell == 32 else [])]
        fonts.append((name, rows, cell * s, cell * s, bits.name, s))
    # the faces alone, large: the boot and setup screens
    fonts.append(("s7_face", [(code, 256, 32, 32, 0, 0, i32.at[code]) for code in faces_], 256, 256, i32.name, 8))
    return tables, fonts


def write_c(tables, fonts, out):
    with open(out, "w") as f:
        f.write("/* Generated by tools/gen_sys7.py from the art in tools/sys7: the System 7 theme's fonts and icons. Do not edit. */\n")
        f.write('#include "bfont.h"\n\n')
        for t in tables:
            f.write(f"static const uint8_t {t.name}_bits[{len(t.data)}] = {{\n")
            for i in range(0, len(t.data), 24):
                f.write("  " + ",".join(f"0x{b:02x}" for b in t.data[i:i + 24]) + ",\n")
            f.write("};\n\n")
        for name, rows, line_h, ascent, bits, s in fonts:
            f.write(f"static const bglyph_t {name}_glyphs[{len(rows)}] = {{\n")
            for r in rows:
                f.write("  { %d, %d, %d, %d, %d, %d, %d },\n" % r)
            f.write("};\n")
            f.write(f"const bfont_t {name} = {{ {name}_glyphs, {len(rows)}, {line_h}, {ascent}, {bits}_bits, {s} }};\n\n")
    size = sum(len(t.data) for t in tables) + sum(len(r) * 16 for _, r, *_ in fonts)
    print(f"wrote {os.path.relpath(out, ROOT)}: {len(fonts)} fonts, about {size // 1024 + 1} KB in flash")


def sheet(tables, fonts, path):
    """A specimen: every text font saying something, then every icon."""
    by = {t.name: t for t in tables}
    W, lines, y = 1872, [], 24
    canvas = {}

    def glyph(font, code, x, y0):
        name, rows, line_h, ascent, bits, s = font
        g = next((r for r in rows if r[0] == code), None)
        if not g:
            return x
        _, adv, w, h, xoff, yoff, off = g
        data, stride = by[bits].data, (w + 1) // 2
        for gy in range(h):
            for gx in range(w):
                b = data[off + gy * stride + gx // 2]
                if (b >> 4 if gx % 2 == 0 else b & 15):
                    for dy in range(s):
                        for dx in range(s):
                            canvas[(x + xoff + gx * s + dx, y0 + yoff + gy * s + dy)] = 0
        return x + adv

    text = {"small": ["The quick brown fox jumps over the lazy dog. 0123456789", "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz",
                      "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ 21.4\x7f \x80 47% humidity"],
            "bold": ["The quick brown fox jumps over the lazy dog.", "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789",
                     "abcdefghijklmnopqrstuvwxyz 21.4\x7f \x80 47%", "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"]}
    for font in fonts:
        name = font[0]
        if "icon" in name:
            x = 24
            for r in font[1]:
                if x + r[1] > W - 24:
                    x, y = 24, y + font[2] + 12
                x = glyph(font, r[0], x, y) + 12
            y += font[2] + 30
        elif name in ("s7_30", "s7_46"):
            for t in text["small" if name == "s7_30" else "bold"]:
                x = 24
                for ch in t:
                    x = glyph(font, ord(ch), x, y)
                y += font[2] + 6
            y += 24
        else:
            x = 24
            for ch in ("06:30 21.4\x7f" if font[5] > 7 else "Partly cloudy 06:30 Sleep"):
                x = glyph(font, ord(ch), x, y)
            y += font[2] + 18
    H = y + 24
    raw = bytearray()
    for yy in range(H):
        raw.append(0)
        raw += bytes(canvas.get((xx, yy), 255) for xx in range(W))
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    print("wrote", path)


if __name__ == "__main__":
    tables, fonts = build()
    write_c(tables, fonts, os.path.join(ROOT, "src", "fonts", "sys7.c"))
    if len(sys.argv) == 3 and sys.argv[1] == "--sheet":
        sheet(tables, fonts, sys.argv[2])
