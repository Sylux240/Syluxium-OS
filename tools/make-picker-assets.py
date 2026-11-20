#!/usr/bin/env python3
"""Artwork for the Syluxium boot picker (boot/picker): a blurred, darkened
copy of the wallpaper as background, entry icons, button icons, the mouse
pointer and an anti-aliased font, written as C arrays to boot/picker/assets.h.
All drawn here (original artwork); the Syluxium mark is keyed out of the
wallpaper like the boot splash's.
Usage: make-picker-assets.py <wallpaper.png> <out.h>"""
import math
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4  # supersampling
src, out = sys.argv[1], sys.argv[2]
FONTS = ["/usr/share/fonts/TTF/DejaVuSans.ttf", "build/rootfs-debian/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]
FONT = next(f for f in FONTS if os.path.exists(f))
BOLD = FONT.replace("DejaVuSans.ttf", "DejaVuSans-Bold.ttf")
wall = Image.open(src).convert("RGB")


def background():
    im = wall.resize((480, 270), Image.LANCZOS).filter(ImageFilter.GaussianBlur(6))
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b = px[x, y]
            k = 0.42  # darker, so icons and text stand out
            px[x, y] = (int(r * k), int(g * k), int(b * k))
    return im


def mark(size):
    """The hexagon S, keyed out of the wallpaper (alpha from brightness)."""
    W, H = wall.size
    c = wall.crop((int(W * 0.672), int(H * 0.11), int(W * 0.895), int(H * 0.40)))
    rgba = Image.new("RGBA", c.size)
    cp, rp = c.load(), rgba.load()
    for y in range(c.height):
        for x in range(c.width):
            r, g, b = cp[x, y]
            a = max(0, min(255, int((min(g, b) - max(110, r * 0.75)) * 2.2)))
            rp[x, y] = (r, g, b, a if a >= 40 else 0)
    rgba = rgba.crop(rgba.getbbox())
    s = size / max(rgba.size)
    rgba = rgba.resize((max(1, int(rgba.width * s)), max(1, int(rgba.height * s))), Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.alpha_composite(rgba, ((size - rgba.width) // 2, (size - rgba.height) // 2))
    return canvas


def rr(d, box, r, **kw):
    d.rounded_rectangle(box, radius=r, **kw)


def icon(kind, size=128):
    S = size * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = S / 128
    if kind == "console":
        rr(d, (10 * u, 22 * u, 118 * u, 106 * u), 10 * u, fill=(28, 30, 36, 255), outline=(150, 160, 175, 255), width=int(3 * u))
        rr(d, (10 * u, 22 * u, 118 * u, 38 * u), 10 * u, fill=(70, 78, 92, 255))
        d.rectangle((10 * u, 30 * u, 118 * u, 38 * u), fill=(70, 78, 92, 255))
        for i, c in enumerate([(235, 90, 80), (240, 190, 60), (90, 200, 90)]):
            d.ellipse(((18 + i * 10) * u, 26 * u, (25 + i * 10) * u, 33 * u), fill=c + (255,))
        f = ImageFont.truetype(BOLD, int(30 * u))
        d.text((22 * u, 52 * u), ">_", font=f, fill=(110, 230, 130, 255))
    elif kind == "safe":
        pts = [(64, 12), (110, 28), (106, 70), (64, 116), (22, 70), (18, 28)]
        d.polygon([(x * u, y * u) for x, y in pts], fill=(40, 110, 220, 255), outline=(200, 225, 255, 255))
        d.line([(42 * u, 62 * u), (58 * u, 80 * u), (88 * u, 44 * u)], fill=(255, 255, 255, 255), width=int(10 * u), joint="curve")
    elif kind in ("windows", "linux", "macos", "efi"):
        # A drive with a coloured badge: other systems, without their logos.
        rr(d, (14 * u, 40 * u, 114 * u, 100 * u), 12 * u, fill=(190, 196, 206, 255), outline=(240, 244, 250, 255), width=int(2 * u))
        rr(d, (14 * u, 78 * u, 114 * u, 100 * u), 12 * u, fill=(150, 156, 168, 255))
        d.ellipse((94 * u, 84 * u, 102 * u, 92 * u), fill=(80, 220, 120, 255))
        badge = {"windows": (0, 120, 215), "linux": (240, 170, 30), "macos": (160, 160, 170), "efi": (110, 110, 120)}[kind]
        letter = {"windows": "W", "linux": "L", "macos": "M", "efi": "?"}[kind]
        d.ellipse((36 * u, 18 * u, 92 * u, 74 * u), fill=badge + (255,), outline=(255, 255, 255, 255), width=int(3 * u))
        f = ImageFont.truetype(BOLD, int(32 * u))
        d.text((64 * u, 46 * u), letter, font=f, fill=(255, 255, 255, 255), anchor="mm")
    elif kind in ("restart", "shutdown"):
        d.ellipse((2 * u, 2 * u, 126 * u, 126 * u), fill=(255, 255, 255, 40), outline=(255, 255, 255, 140), width=int(4 * u))
        w = int(10 * u)
        if kind == "shutdown":
            d.arc((34 * u, 34 * u, 94 * u, 94 * u), start=-60, end=240, fill=(255, 255, 255, 255), width=w)
            d.line([(64 * u, 24 * u), (64 * u, 62 * u)], fill=(255, 255, 255, 255), width=w)
        else:
            d.arc((34 * u, 34 * u, 94 * u, 94 * u), start=-30, end=280, fill=(255, 255, 255, 255), width=w)
            a = math.radians(-30)
            cx, cy = 64 + 30 * math.cos(a), 64 + 30 * math.sin(a)
            d.polygon([((cx - 12) * u, (cy - 18) * u), ((cx + 14) * u, (cy - 6) * u), ((cx - 4) * u, (cy + 10) * u)], fill=(255, 255, 255, 255))
    im = im.resize((size, size), Image.LANCZOS)
    if kind == "syluxium":
        im = mark(size)
    return im


def cursor():
    S = 24 * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pts = [(1, 1), (1, 19), (6, 14), (10, 22), (13, 21), (9, 13), (16, 13)]
    d.polygon([(x * SS, y * SS) for x, y in pts], fill=(255, 255, 255, 255), outline=(0, 0, 0, 255))
    d.line([(x * SS, y * SS) for x, y in pts + [pts[0]]], fill=(0, 0, 0, 255), width=SS)
    return im.resize((24, 24), Image.LANCZOS)


def argb(im):
    data = im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata()
    return [(a << 24) | (r << 16) | (g << 8) | b for r, g, b, a in data]


def write_array(f, name, w, h, vals, fmt="0x%08x"):
    f.write(f"static const unsigned int {name}_W = {w}, {name}_H = {h};\n")
    f.write(f"static const unsigned int {name}[] = {{\n")
    for i in range(0, len(vals), 10):
        f.write(",".join(fmt % v for v in vals[i:i + 10]) + ",\n")
    f.write("};\n")


def font_atlas(f, name, path, px):
    font = ImageFont.truetype(path, px)
    asc, desc = font.getmetrics()
    glyphs, data = [], []
    for c in range(32, 127):
        ch = chr(c)
        bbox = font.getbbox(ch)
        adv = int(round(font.getlength(ch)))
        w, h = max(0, bbox[2] - bbox[0]), max(0, bbox[3] - bbox[1])
        g = Image.new("L", (max(1, w), max(1, h)), 0)
        ImageDraw.Draw(g).text((-bbox[0], -bbox[1]), ch, font=font, fill=255)
        glyphs.append((adv, w, h, bbox[0], bbox[1], len(data)))
        if w and h:
            data += list(g.get_flattened_data() if hasattr(g, "get_flattened_data") else g.getdata())
    f.write(f"static const int {name}_ASCENT = {asc}, {name}_HEIGHT = {asc + desc};\n")
    f.write(f"static const struct glyph {name}_GLYPHS[] = {{\n")
    for adv, w, h, x, y, off in glyphs:
        f.write(f"{{{adv},{w},{h},{x},{y},{off}}},\n")
    f.write("};\n")
    f.write(f"static const unsigned char {name}_ALPHA[] = {{\n")
    for i in range(0, len(data), 24):
        f.write(",".join(str(v) for v in data[i:i + 24]) + ",\n")
    f.write("};\n")


with open(out, "w") as f:
    f.write("/* Generated by tools/make-picker-assets.py - do not edit. */\n#pragma once\n")
    f.write("struct glyph { short advance, w, h, x, y; unsigned int offset; };\n")
    bg = background()
    write_array(f, "BG", bg.width, bg.height,
                [(r << 16) | (g << 8) | b for r, g, b in (bg.get_flattened_data() if hasattr(bg, "get_flattened_data") else bg.getdata())])
    for k in ("syluxium", "console", "safe", "windows", "linux", "macos", "efi"):
        im = icon(k)
        write_array(f, "ICON_" + k.upper(), im.width, im.height, argb(im))
    for k in ("restart", "shutdown"):
        im = icon(k, 44)
        write_array(f, "BTN_" + k.upper(), im.width, im.height, argb(im))
    c = cursor()
    write_array(f, "CURSOR", c.width, c.height, argb(c))
    font_atlas(f, "FONT", FONT, 17)
    font_atlas(f, "FONTB", BOLD, 26)
print("assets written to", out)
