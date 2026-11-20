#!/usr/bin/env python3
"""Draw Syluxium's XP-era desktop artwork (all original drawings, only the
general *style* of that era: glossy blue title bars, round-cornered buttons,
blue/white welcome screens).

Writes, under <rootfs> (default: rootfs/):
  opt/trinity/share/apps/deKorator/themes/Syluxium-theme/   window frames (twin deKorator)
  opt/trinity/share/apps/kicker/pics/syluxium_menu_top_*.png  Start menu header
  opt/trinity/share/apps/tdm/faces/.default.face.icon        default user picture
  opt/trinity/share/apps/ksplash/Themes/Syluxium/            session start splash
  opt/trinity/share/apps/tdm/themes/syluxium/                login screen (TDM theme)
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4  # supersampling factor for anti-aliased shapes


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def gradient(stops, t):
    for i in range(len(stops) - 1):
        if stops[i][0] <= t <= stops[i + 1][0]:
            a, b = stops[i], stops[i + 1]
            return lerp(a[1], b[1], (t - a[0]) / (b[0] - a[0]))
    return stops[-1][1]


def font(size, bold=True):
    names = (["DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"] if bold
             else ["DejaVuSans.ttf", "LiberationSans-Regular.ttf"])
    for d in ("/usr/share/fonts/TTF", "/usr/share/fonts/truetype/dejavu",
              "/usr/share/fonts/liberation", "/usr/share/fonts/truetype/liberation"):
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def save(im, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path, format="PNG")


# ---------------------------------------------------------------------------
# Window decoration (twin deKorator theme)
# ---------------------------------------------------------------------------
TITLE_H = 29
FRAME = 4
CORNER_W = 6

ACTIVE_TITLE = [(0.00, (0, 44, 170)), (0.04, (72, 150, 255)), (0.10, (36, 124, 255)),
                (0.22, (6, 96, 246)), (0.50, (0, 82, 228)), (0.80, (2, 94, 242)),
                (0.93, (0, 76, 218)), (1.00, (0, 52, 178))]
INACTIVE_TINT = (150, 172, 230)


def inactive(c, amount=0.62):
    return lerp(c, INACTIVE_TINT, amount)


def title_col(y, act):
    c = gradient(ACTIVE_TITLE, y / (TITLE_H - 1))
    return c if act else inactive(c)


FRAME_COLS = [(0, 40, 160), (16, 98, 240), (0, 84, 228), (0, 58, 196)]  # outside -> inside


def frame_col(i, act):
    c = FRAME_COLS[i]
    return c if act else inactive(c)


def title_tile(w, act):
    im = Image.new("RGB", (w, TITLE_H))
    px = im.load()
    for y in range(TITLE_H):
        c = title_col(y, act)
        for x in range(w):
            px[x, y] = c
    return im


def title_corner(left, act):
    """Left/right end of the title bar: the gradient plus the frame's edge
    columns (the round corner itself is cut out by the mask)."""
    im = title_tile(CORNER_W, act)
    px = im.load()
    for y in range(TITLE_H):
        for i in range(2):
            x = i if left else CORNER_W - 1 - i
            base = title_col(y, act)
            px[x, y] = frame_col(0, act) if i == 0 else lerp(base, (255, 255, 255) if act else base, 0.18)
    # lower rows blend into the side frame
    for y in range(TITLE_H - 3, TITLE_H):
        for i in range(FRAME):
            x = i if left else CORNER_W - 1 - i
            px[x, y] = frame_col(i, act)
    return im


def side_frame(left, h, act):
    im = Image.new("RGB", (FRAME, h))
    px = im.load()
    for y in range(h):
        for i in range(FRAME):
            px[i if left else FRAME - 1 - i, y] = frame_col(i, act)
    return im


def bottom_frame(w, act, corner=None):
    im = Image.new("RGB", (w, FRAME))
    px = im.load()
    for x in range(w):
        for j in range(FRAME):
            px[x, FRAME - 1 - j] = frame_col(j, act)
    if corner:  # the outer column on the left/right end
        for j in range(FRAME):
            for i in range(FRAME):
                x = i if corner == "left" else w - 1 - i
                y = FRAME - 1 - j
                px[x, y] = frame_col(min(i, j), act)
    return im


def rounded_mask(w, h, r, fill=255):
    big = Image.new("L", (w * SS, h * SS), 0)
    ImageDraw.Draw(big).rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), r * SS, fill=fill)
    return big.resize((w, h), Image.LANCZOS)


BTN = 21  # XP-sized caption buttons
BTN_GAP = 2


def button(kind, state, act):
    """A caption button: rounded square, thin light border, glossy fill and a
    white symbol. kind: min/max/restore/close/help/menu/sticky/stickydown/
    above/abovedown/below/belowdown/shade/shadedown."""
    w = BTN + BTN_GAP
    im = Image.new("RGBA", (w, BTN), (0, 0, 0, 0))
    if kind == "menu":  # the application icon is drawn instead
        return Image.new("RGBA", (18, BTN), (0, 0, 0, 0))
    S = BTN * SS
    big = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    if kind == "close":
        top, bottom, edge = (238, 132, 100), (206, 60, 30), (255, 236, 228)
    else:
        top, bottom, edge = (112, 168, 255), (22, 86, 236), (236, 244, 255)
    if state == "hover":
        top, bottom = lerp(top, (255, 255, 255), 0.25), lerp(bottom, (255, 255, 255), 0.18)
    elif state == "press":
        top, bottom = lerp(bottom, (0, 0, 0), 0.25), lerp(top, (0, 0, 0), 0.2)
    if not act:
        top, bottom, edge = inactive(top, 0.55), inactive(bottom, 0.55), inactive(edge, 0.3)
    fill = Image.new("RGBA", (S, S))
    fp = fill.load()
    for y in range(S):
        for x in range(S):
            t = (x * 0.35 + y * 0.65) / S
            c = lerp(top, bottom, t)
            # soft highlight in the upper left
            d = math.hypot(x - S * 0.3, y - S * 0.25) / S
            c = lerp(c, (255, 255, 255), max(0.0, 0.35 - d) * (0.9 if state != "press" else 0.3))
            fp[x, y] = c + (255,)
    r = 3 * SS
    border = Image.new("L", (S, S), 0)
    ImageDraw.Draw(border).rounded_rectangle((0, 0, S - 1, S - 1), r, fill=255)
    inner = Image.new("L", (S, S), 0)
    ImageDraw.Draw(inner).rounded_rectangle((SS, SS, S - 1 - SS, S - 1 - SS), r - SS, fill=255)
    big.paste(edge + (235 if act else 200,), (0, 0), border)
    big.paste(fill, (0, 0), inner)
    g = ImageDraw.Draw(big)
    white = (255, 255, 255, 255) if act else (240, 244, 252, 255)
    u = SS  # one pixel at the final size

    def box(x0, y0, x1, y1):
        g.rectangle((x0 * u, y0 * u, (x1 + 1) * u - 1, (y1 + 1) * u - 1), fill=white)

    def frame_box(x0, y0, x1, y1, top_w=2):
        box(x0, y0, x1, y0 + top_w - 1)
        box(x0, y0, x0, y1)
        box(x1, y0, x1, y1)
        box(x0, y1, x1, y1)

    if kind == "min":
        box(6, 13, 12, 15)
    elif kind == "max":
        frame_box(5, 5, 15, 15, 3)
    elif kind == "restore":
        frame_box(8, 4, 16, 12, 2)
        g.rectangle((5 * u, 8 * u, 13 * u - 1, 17 * u - 1), fill=fill.getpixel((9 * u, 12 * u)))
        frame_box(4, 8, 12, 16, 2)
    elif kind == "close":
        w2 = 2.6 * u
        g.line((6 * u, 6 * u, 15 * u, 15 * u), fill=white, width=int(w2))
        g.line((15 * u, 6 * u, 6 * u, 15 * u), fill=white, width=int(w2))
    elif kind == "help":
        f = font(15 * u)
        g.text((S / 2, S / 2 + u), "?", font=f, fill=white, anchor="mm")
    elif kind in ("sticky", "stickydown"):
        g.ellipse((7 * u, 7 * u, 14 * u, 14 * u), fill=white if kind == "stickydown" else None,
                  outline=white, width=2 * u)
    elif kind in ("above", "abovedown"):
        g.polygon([(10.5 * u, 5 * u), (16 * u, 12 * u), (5 * u, 12 * u)], fill=white)
        if kind == "abovedown":
            box(5, 14, 16, 15)
    elif kind in ("below", "belowdown"):
        g.polygon([(5 * u, 9 * u), (16 * u, 9 * u), (10.5 * u, 16 * u)], fill=white)
        if kind == "belowdown":
            box(5, 5, 16, 6)
    elif kind in ("shade", "shadedown"):
        box(5, 6, 15, 8)
        if kind == "shadedown":
            g.polygon([(6 * u, 11 * u), (15 * u, 11 * u), (10.5 * u, 16 * u)], fill=white)
    small = big.resize((BTN, BTN), Image.LANCZOS)
    im.paste(small, (BTN_GAP, 0), small)
    return im


def make_window_theme(root):
    base = os.path.join(root, "opt/trinity/share/apps/deKorator/themes/Syluxium-theme")
    deco = os.path.join(base, "deco")
    for act in (True, False):
        sfx = "Bg.png" if act else "InactiveBg.png"
        parts = {
            "leftUpperCornerFrame": title_corner(True, act),
            "rightUpperCornerFrame": title_corner(False, act),
            "buttonsLeft": title_tile(8, act),
            "titleLeft": title_tile(8, act),
            "titleMid": title_tile(32, act),
            "titleRight": title_tile(8, act),
            "buttonsRight": title_tile(8, act),
            "leftUpperFrame": side_frame(True, 8, act),
            "leftMidFrame": side_frame(True, 16, act),
            "leftLowerCornerFrame": side_frame(True, 8, act),
            "rightUpperFrame": side_frame(False, 8, act),
            "rightMidFrame": side_frame(False, 16, act),
            "rightLowerCornerFrame": side_frame(False, 8, act),
            "bottomLeftFrame": bottom_frame(FRAME, act, "left"),
            "bottomMidFrame": bottom_frame(16, act),
            "bottomRightFrame": bottom_frame(FRAME, act, "right"),
        }
        for name, im in parts.items():
            save(im, os.path.join(deco, name + sfx))
    # Masks: black pixels are cut away (round top corners, radius 6).
    r = 6
    m = rounded_mask(2 * r + 2, 2 * r + 2, r)
    tl = Image.new("1", (r, r), 0)
    for y in range(r):
        for x in range(r):
            tl.putpixel((x, y), 1 if m.getpixel((x, y)) >= 128 else 0)
    # PNG: 0 = black = cut, so invert: outside the rounding -> black
    tlimg = Image.new("RGB", (r, r), (255, 255, 255))
    for y in range(r):
        for x in range(r):
            if not tl.getpixel((x, y)):
                tlimg.putpixel((x, y), (0, 0, 0))
    save(tlimg, os.path.join(base, "masks/topLeftCornerBitmap.png"))
    save(tlimg.transpose(Image.FLIP_LEFT_RIGHT), os.path.join(base, "masks/topRightCornerBitmap.png"))
    names = {"min": "buttonMin", "max": "buttonMax", "restore": "buttonRestore",
             "close": "buttonClose", "help": "buttonHelp", "menu": "buttonMenu",
             "sticky": "buttonSticky", "stickydown": "buttonStickyDown",
             "above": "buttonAbove", "abovedown": "buttonAboveDown",
             "below": "buttonBelow", "belowdown": "buttonBelowDown",
             "shade": "buttonShade", "shadedown": "buttonShadeDown"}
    for kind, name in names.items():
        for state, sub, suffix in (("normal", "normal", ""), ("hover", "hover", "Hover"), ("press", "press", "Press")):
            for act in (True, False):
                im = button(kind, state, act)
                save(im, os.path.join(base, "buttons", sub, name + suffix + ("" if act else "Inactive") + ".png"))
    # the sticky "press" image name used by deKorator for the up state
    for act in (True, False):
        save(button("sticky", "press", act), os.path.join(base, "buttons/press/buttonStickyDownPress" + ("" if act else "Inactive") + ".png"))


# ---------------------------------------------------------------------------
# User picture (TDM user list, Start menu header, splash)
# ---------------------------------------------------------------------------
def user_picture(size=64):
    """An original "user tile": a stylised green hill under a sunny sky."""
    S = size * SS
    im = Image.new("RGB", (S, S))
    px = im.load()
    for y in range(S):
        c = lerp((96, 160, 245), (200, 228, 255), y / (S * 0.62))
        for x in range(S):
            px[x, y] = c
    g = ImageDraw.Draw(im)
    g.ellipse((S * 0.58, S * 0.12, S * 0.86, S * 0.40), fill=(255, 214, 80))
    g.ellipse((-S * 0.3, S * 0.55, S * 1.1, S * 1.6), fill=(70, 160, 60))
    g.ellipse((S * 0.35, S * 0.68, S * 1.5, S * 1.7), fill=(46, 130, 44))
    im = im.resize((size, size), Image.LANCZOS)
    return im


def framed_picture(size, border=3):
    pic = user_picture(size - 2 * border).convert("RGBA")
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste((255, 255, 255, 255), (0, 0), rounded_mask(size, size, 5))
    mask = rounded_mask(size - 2 * border, size - 2 * border, 3)
    out.paste(pic, (border, border), mask)
    return out


# ---------------------------------------------------------------------------
# Start menu header (Kicker "top side" pixmaps)
# ---------------------------------------------------------------------------
MENU_TOP_H = 56


def menu_top_col(y):
    stops = [(0.0, (150, 192, 255)), (0.06, (40, 116, 230)), (0.5, (16, 88, 214)),
             (0.9, (12, 76, 196)), (0.94, (240, 170, 70)), (1.0, (200, 110, 30))]
    return gradient(stops, y / (MENU_TOP_H - 1))


def make_menu_top(root):
    pics = os.path.join(root, "opt/trinity/share/apps/kicker/pics")
    tile = Image.new("RGB", (16, MENU_TOP_H))
    for y in range(MENU_TOP_H):
        for x in range(16):
            tile.putpixel((x, y), menu_top_col(y))
    save(tile, os.path.join(pics, "syluxium_menu_top_tile.png"))
    left = Image.new("RGB", (220, MENU_TOP_H))
    for y in range(MENU_TOP_H):
        for x in range(220):
            left.putpixel((x, y), menu_top_col(y))
    pic = framed_picture(44)
    left.paste(pic, (7, 4), pic)
    g = ImageDraw.Draw(left)
    f = font(15)
    g.text((61, 25), "Syluxium", font=f, fill=(0, 30, 110), anchor="lm")
    g.text((60, 24), "Syluxium", font=f, fill=(255, 255, 255), anchor="lm")
    save(left, os.path.join(pics, "syluxium_menu_top_left.png"))
    right = Image.new("RGB", (8, MENU_TOP_H))
    for y in range(MENU_TOP_H):
        for x in range(8):
            right.putpixel((x, y), menu_top_col(y))
    save(right, os.path.join(pics, "syluxium_menu_top_right.png"))


# ---------------------------------------------------------------------------
# Welcome screen background (splash + login screen)
# ---------------------------------------------------------------------------
def welcome_background(w=1024, h=768, separator=True):
    band = int(h * 0.1)
    im = Image.new("RGB", (w, h))
    px = im.load()
    mid_a, mid_b = (88, 128, 222), (64, 104, 206)
    for y in range(h):
        for x in range(w):
            if y < band:
                c = lerp((0, 40, 140), (0, 52, 164), y / band)
            elif y >= h - band:
                c = lerp((0, 52, 164), (0, 36, 128), (y - (h - band)) / band)
            else:
                c = lerp(mid_a, mid_b, (y - band) / (h - 2 * band))
                d = math.hypot(x / w * 1.3, (y - band) / h * 1.9)
                c = lerp((206, 222, 250), c, min(1.0, d / 0.75) ** 0.8)
            px[x, y] = c
    g = ImageDraw.Draw(im)
    for x in range(w):  # light line under the top band, warm line over the bottom band
        t = abs(x - w * 0.35) / (w * 0.65)
        px[x, band] = lerp((200, 220, 255), (70, 110, 210), t)
        px[x, band + 1] = lerp((120, 160, 240), (70, 110, 210), t)
        px[x, h - band - 2] = lerp((255, 196, 110), (70, 110, 210), t)
        px[x, h - band - 1] = lerp((240, 150, 60), (60, 90, 190), t)
    if separator:
        for y in range(band + 40, h - band - 40):
            t = abs(y - h / 2) / (h / 2 - band - 40)
            c = lerp((190, 210, 250), px[w // 2, y], t)
            px[w // 2, y] = c
    return im


def make_splash(root):
    d = os.path.join(root, "opt/trinity/share/apps/ksplash/Themes/Syluxium")
    bg = welcome_background()
    save(bg, os.path.join(d, "Background.png"))
    # the Redmond engine prefers Background-<w>x<h>.png for the screen size
    for w, h in ((1280, 800), (1280, 1024), (1366, 768), (1920, 1080)):
        save(welcome_background(w, h), os.path.join(d, f"Background-{w}x{h}.png"))
    prev = bg.resize((320, 240), Image.LANCZOS)
    g = ImageDraw.Draw(prev)
    g.text((150, 120), "welcome", font=font(18), fill=(255, 255, 255), anchor="rm")
    save(prev, os.path.join(d, "Preview.png"))


def make_tdm_theme(root):
    d = os.path.join(root, "opt/trinity/share/apps/tdm/themes/syluxium")
    bg = welcome_background(1280, 800)
    save(bg, os.path.join(d, "background.png"))
    # "Turn off computer" button (red rounded square with a power symbol)
    for state, k in (("normal", 0.0), ("prelight", 0.2)):
        S = 26 * SS
        big = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        g = ImageDraw.Draw(big)
        top, bottom = lerp((236, 120, 80), (255, 255, 255), k), lerp((196, 56, 24), (255, 255, 255), k)
        for y in range(S):
            g.line((0, y, S, y), fill=lerp(top, bottom, y / S) + (255,))
        m = Image.new("L", (S, S), 0)
        ImageDraw.Draw(m).rounded_rectangle((0, 0, S - 1, S - 1), 5 * SS, fill=255)
        out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        out.paste(big, (0, 0), m)
        g = ImageDraw.Draw(out)
        u = SS
        g.arc((6 * u, 6 * u, 20 * u, 20 * u), 300, 240, fill=(255, 255, 255, 255), width=int(2.4 * u))
        g.line((13 * u, 4 * u, 13 * u, 13 * u), fill=(255, 255, 255, 255), width=int(2.4 * u))
        save(out.resize((26, 26), Image.LANCZOS), os.path.join(d, f"system_{state}.png"))
    for state, col in (("normal", (36, 160, 50)), ("prelight", (70, 196, 80))):
        S = 22 * SS
        out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        m = Image.new("L", (S, S), 0)
        ImageDraw.Draw(m).rounded_rectangle((0, 0, S - 1, S - 1), 4 * SS, fill=255)
        fill = Image.new("RGBA", (S, S), col + (255,))
        out.paste(fill, (0, 0), m)
        g = ImageDraw.Draw(out)
        u = SS
        g.polygon([(6 * u, 9 * u), (12 * u, 9 * u), (12 * u, 5 * u), (18 * u, 11 * u),
                   (12 * u, 17 * u), (12 * u, 13 * u), (6 * u, 13 * u)], fill=(255, 255, 255, 255))
        save(out.resize((22, 22), Image.LANCZOS), os.path.join(d, f"login_{state}.png"))
    # the name on the left half, bold italic with a soft shadow
    f = font(40)
    w = int(f.getlength("Syluxium")) + 24
    logo = Image.new("RGBA", (w, 64), (0, 0, 0, 0))
    shadow = Image.new("RGBA", logo.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((14, 34), "Syluxium", font=f, fill=(10, 30, 110, 170), anchor="lm")
    logo.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(2)))
    ImageDraw.Draw(logo).text((12, 32), "Syluxium", font=f, fill=(255, 255, 255, 255), anchor="lm")
    logo = logo.transform(logo.size, Image.AFFINE, (1, 0.18, -6, 0, 1, 0), Image.BICUBIC)  # slant
    save(logo, os.path.join(d, "logo.png"))
    prev = bg.resize((320, 200), Image.LANCZOS)
    save(prev, os.path.join(d, "screenshot.png"))


def start_icon(size=22):
    """The Start button's icon: Syluxium's hexagon mark (as on the
    wallpaper), white on a green-to-cyan gem."""
    S = size * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    c = S / 2
    r = S * 0.48
    hexa = [(c + r * math.cos(math.radians(a)), c + r * math.sin(math.radians(a))) for a in range(-90, 270, 60)]
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).polygon(hexa, fill=255)
    fill = Image.new("RGBA", (S, S))
    for y in range(S):
        ImageDraw.Draw(fill).line((0, y, S, y), fill=lerp((150, 240, 255), (20, 120, 200), y / S) + (255,))
    im.paste(fill, (0, 0), m)
    g = ImageDraw.Draw(im)
    g.polygon(hexa, outline=(255, 255, 255, 255), width=int(1.2 * SS))
    g.text((c, c + SS * 0.5), "S", font=font(int(S * 0.62)), fill=(255, 255, 255, 255), anchor="mm")
    return im.resize((size, size), Image.LANCZOS)


def shutdown_picture(w, h):
    """Picture of the Turn Off dialog: the welcome-screen blue with a big
    power symbol and the Syluxium mark."""
    im = welcome_background(w * 2, h * 2, separator=False).crop((0, 0, w * 2, h * 2))
    im = im.resize((w, h), Image.LANCZOS).convert("RGBA")
    S = 80 * SS
    sym = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(sym)
    g.rounded_rectangle((0, 0, S - 1, S - 1), 14 * SS, fill=(220, 80, 40, 255), outline=(255, 255, 255, 255), width=3 * SS)
    u = SS
    g.arc((18 * u, 20 * u, 62 * u, 64 * u), 300, 240, fill=(255, 255, 255, 255), width=7 * u)
    g.line((40 * u, 12 * u, 40 * u, 40 * u), fill=(255, 255, 255, 255), width=7 * u)
    sym = sym.resize((80, 80), Image.LANCZOS)
    im.alpha_composite(sym, ((w - 80) // 2, h // 2 - 60))
    icon = start_icon(32)
    im.alpha_composite(icon, (8, h - 40))
    return im


def main(root):
    pics = os.path.join(root, "opt/trinity/share/apps/ksmserver/pics")
    save(shutdown_picture(188, 300), os.path.join(pics, "shutdownkonq.png"))
    save(shutdown_picture(148, 217).convert("RGB"), os.path.join(pics, "shutdown.png"))
    save(start_icon(), os.path.join(root, "opt/trinity/share/apps/kicker/pics/syluxium_start.png"))
    make_window_theme(root)
    make_menu_top(root)
    face = framed_picture(64)
    save(face, os.path.join(root, "opt/trinity/share/apps/tdm/faces/.default.face.icon"))
    make_splash(root)
    make_tdm_theme(root)
    print(f"XP-style artwork written under {root}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "rootfs")
