#!/usr/bin/env python3
"""Draw Syluxium's "Dusk" desktop artwork: deep navy and indigo panels, the
logo's cyan as the accent, lavender and cloud-pink highlights, softly rounded
shapes. Colours from the Dusk palette:

  Night #141A3A (panels)   Indigo #1D2552 (title bars)   Cyan #6FE3F0 (accent)
  Lavender #B9A3E3 (avatars)   Cloud pink #F29AB6 / #E8708F (power, close)
  Sun gold #F6C96B (files, alerts)   Starlight #EEF2FF (text)

Reads the artwork in rootfs/usr/share/syluxium/dusk/ (wallpaper, login
background, logos) and writes, under <rootfs> (default: rootfs/):
  opt/trinity/share/apps/deKorator/themes/Syluxium-theme/   window frames (twin deKorator)
  opt/trinity/share/apps/kicker/pics/                       launcher icon, menu header
  opt/trinity/share/apps/kicker/tiles/syluxium_dusk_*       launcher button tiles
  opt/trinity/share/apps/kicker/wallpapers/syluxium_taskbar.png  taskbar (transparency off)
  opt/trinity/share/apps/tdm/faces/.default.face.icon        default user picture
  opt/trinity/share/apps/ksplash/Themes/Syluxium/            session start splash
  opt/trinity/share/apps/tdm/themes/syluxium/                login screen artwork
  opt/trinity/share/apps/ksmserver/pics/                     shut-down dialog pictures
  opt/trinity/share/wallpapers/syluxium-default.jpg          desktop wallpaper
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4  # supersampling for anti-aliased shapes

NIGHT = (20, 26, 58)
INDIGO = (29, 37, 82)
GLASS = (18, 23, 54)
CYAN = (111, 227, 240)
LAVENDER = (185, 163, 227)
PINK = (242, 154, 182)
CLOSE = (232, 112, 143)
STAR = (238, 242, 255)
SOFT = (223, 228, 255)


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def font(size, bold=True):
    names = (["Inter-SemiBold.otf", "Inter-SemiBold.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"] if bold
             else ["Inter-Regular.otf", "Inter-Regular.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf"])
    for d in ("/usr/share/fonts/opentype/inter", "/usr/share/fonts/truetype/inter", "/usr/share/fonts/inter",
              "/usr/share/fonts/OTF", "/usr/share/fonts/TTF", "/usr/share/fonts/truetype/dejavu",
              "/usr/share/fonts/liberation", "/usr/share/fonts/truetype/liberation"):
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def save(im, path, fmt="PNG", **kw):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path, format=fmt, **kw)


def rounded(size, r, fill, outline=None, width=0):
    w, h = size
    big = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    ImageDraw.Draw(big).rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), r * SS, fill=fill,
                                          outline=outline, width=width * SS)
    return big.resize((w, h), Image.LANCZOS)


# ---------------------------------------------------------------------------
# Window frames (twin deKorator)
# ---------------------------------------------------------------------------
TITLE_H = 32
FRAME = 3
CORNER_W = 10
ACT_TITLE, INACT_TITLE = INDIGO, (24, 30, 64)
ACT_EDGE, INACT_EDGE = lerp(INDIGO, CYAN, 0.30), lerp((24, 30, 64), CYAN, 0.10)


def title_rows(act):
    """The title bar, top to bottom: a faint light top edge, flat indigo,
    and the cyan line under it (stronger on the active window)."""
    base = ACT_TITLE if act else INACT_TITLE
    rows = []
    for y in range(TITLE_H):
        if y == 0:
            c = lerp(base, CYAN, 0.30 if act else 0.10)
        elif y == TITLE_H - 1:
            c = lerp(base, CYAN, 0.45 if act else 0.12)
        else:
            c = lerp(lerp(base, (255, 255, 255), 0.035), base, y / TITLE_H)
        rows.append(c)
    return rows


def title_tile(w, act):
    im = Image.new("RGB", (w, TITLE_H))
    for y, c in enumerate(title_rows(act)):
        ImageDraw.Draw(im).line((0, y, w, y), fill=c)
    return im


def title_corner(left, act):
    im = title_tile(CORNER_W, act)
    px = im.load()
    edge = ACT_EDGE if act else INACT_EDGE
    for y in range(TITLE_H):
        px[0 if left else CORNER_W - 1, y] = edge
    return im


def frame_cols(act):
    edge = ACT_EDGE if act else INACT_EDGE
    body = (27, 33, 72) if act else (22, 27, 58)
    return [edge, body, body]  # outside -> inside


def side_frame(left, h, act):
    im = Image.new("RGB", (FRAME, h))
    px = im.load()
    cols = frame_cols(act)
    for y in range(h):
        for i in range(FRAME):
            px[i if left else FRAME - 1 - i, y] = cols[i]
    return im


def bottom_frame(w, act, corner=None):
    im = Image.new("RGB", (w, FRAME))
    px = im.load()
    cols = frame_cols(act)
    for x in range(w):
        for j in range(FRAME):
            px[x, FRAME - 1 - j] = cols[j]
    if corner:
        for j in range(FRAME):
            for i in range(FRAME):
                px[i if corner == "left" else w - 1 - i, FRAME - 1 - j] = cols[min(i, j)]
    return im


BTN = 24
BTN_GAP = 6


def button(kind, state, act):
    """Caption buttons: soft rounded squares (8% white) with thin line
    symbols; close is cloud pink."""
    w = BTN + BTN_GAP
    im = Image.new("RGBA", (w, TITLE_H), (0, 0, 0, 0))
    if kind == "menu":
        return Image.new("RGBA", (22, TITLE_H), (0, 0, 0, 0))
    base = ACT_TITLE if act else INACT_TITLE
    if kind == "close":
        fill = CLOSE if act else lerp(CLOSE, base, 0.55)
        sym = (255, 255, 255)
    else:
        fill = lerp(base, (255, 255, 255), 0.08)
        sym = SOFT if act else lerp(SOFT, base, 0.45)
    if state == "hover":
        fill = lerp(fill, (255, 255, 255), 0.16) if kind == "close" else lerp(base, CYAN, 0.28)
        if kind != "close":
            sym = (255, 255, 255)
    elif state == "press":
        fill = lerp(fill, (0, 0, 0), 0.22)
    S = BTN * SS
    big = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(big)
    g.rounded_rectangle((0, 0, S - 1, S - 1), 7 * SS, fill=fill + (255,))
    u = SS
    lw = int(1.7 * u)
    s = sym + (255,)
    c = S / 2
    if kind == "min":
        g.line((c - 5 * u, c, c + 5 * u, c), fill=s, width=lw)
    elif kind == "max":
        g.rounded_rectangle((c - 5 * u, c - 5 * u, c + 5 * u, c + 5 * u), 2 * u, outline=s, width=lw)
    elif kind == "restore":
        g.rounded_rectangle((c - 2.5 * u, c - 6 * u, c + 6 * u, c + 2.5 * u), 2 * u, outline=s, width=lw)
        g.rounded_rectangle((c - 6 * u, c - 2.5 * u, c + 2.5 * u, c + 6 * u), 2 * u, outline=s, width=lw,
                            fill=fill + (255,))
    elif kind == "close":
        g.line((c - 4.5 * u, c - 4.5 * u, c + 4.5 * u, c + 4.5 * u), fill=s, width=lw)
        g.line((c + 4.5 * u, c - 4.5 * u, c - 4.5 * u, c + 4.5 * u), fill=s, width=lw)
    elif kind == "help":
        g.text((c, c + u), "?", font=font(15 * u), fill=s, anchor="mm")
    elif kind in ("sticky", "stickydown"):
        g.ellipse((c - 4 * u, c - 4 * u, c + 4 * u, c + 4 * u), fill=s if kind == "stickydown" else None,
                  outline=s, width=lw)
    elif kind in ("above", "abovedown"):
        g.line((c - 5 * u, c + 2 * u, c, c - 3 * u, c + 5 * u, c + 2 * u), fill=s, width=lw, joint="curve")
        if kind == "abovedown":
            g.line((c - 5 * u, c + 5 * u, c + 5 * u, c + 5 * u), fill=s, width=lw)
    elif kind in ("below", "belowdown"):
        g.line((c - 5 * u, c - 2 * u, c, c + 3 * u, c + 5 * u, c - 2 * u), fill=s, width=lw, joint="curve")
        if kind == "belowdown":
            g.line((c - 5 * u, c - 5 * u, c + 5 * u, c - 5 * u), fill=s, width=lw)
    elif kind in ("shade", "shadedown"):
        g.line((c - 5 * u, c - 3 * u, c + 5 * u, c - 3 * u), fill=s, width=lw)
        if kind == "shadedown":
            g.line((c - 4 * u, c + 1 * u, c, c + 5 * u, c + 4 * u, c + 1 * u), fill=s, width=lw)
    small = big.resize((BTN, BTN), Image.LANCZOS)
    im.paste(small, (BTN_GAP // 2, (TITLE_H - BTN) // 2), small)
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
    # Masks: black is cut away (round top corners, radius 10).
    r = 10
    m = rounded((2 * r + 2, 2 * r + 2), r, (255, 255, 255, 255)).split()[3]
    tl = Image.new("RGB", (r, r), (255, 255, 255))
    for y in range(r):
        for x in range(r):
            if m.getpixel((x, y)) < 128:
                tl.putpixel((x, y), (0, 0, 0))
    save(tl, os.path.join(base, "masks/topLeftCornerBitmap.png"))
    save(tl.transpose(Image.FLIP_LEFT_RIGHT), os.path.join(base, "masks/topRightCornerBitmap.png"))
    names = {"min": "buttonMin", "max": "buttonMax", "restore": "buttonRestore",
             "close": "buttonClose", "help": "buttonHelp", "menu": "buttonMenu",
             "sticky": "buttonSticky", "stickydown": "buttonStickyDown",
             "above": "buttonAbove", "abovedown": "buttonAboveDown",
             "below": "buttonBelow", "belowdown": "buttonBelowDown",
             "shade": "buttonShade", "shadedown": "buttonShadeDown"}
    for kind, name in names.items():
        for state, sub, suffix in (("normal", "normal", ""), ("hover", "hover", "Hover"), ("press", "press", "Press")):
            for act in (True, False):
                save(button(kind, state, act),
                     os.path.join(base, "buttons", sub, name + suffix + ("" if act else "Inactive") + ".png"))
    for act in (True, False):
        save(button("sticky", "press", act),
             os.path.join(base, "buttons/press/buttonStickyDownPress" + ("" if act else "Inactive") + ".png"))


# ---------------------------------------------------------------------------
# Launcher button, menu header, taskbar, user picture
# ---------------------------------------------------------------------------
def logo(art, name, height):
    im = Image.open(os.path.join(art, name)).convert("RGBA")
    return im.resize((max(1, round(im.width * height / im.height)), height), Image.LANCZOS)


def launcher_tile(size, pressed):
    """The launcher button: a rounded square in a cyan wash with a cyan
    border and a soft glow (the hex logo is the button's icon)."""
    w, h = size + 4, size
    S = SS
    big = Image.new("RGBA", (w * S, h * S), GLASS + (0,))
    glow = Image.new("RGBA", big.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).rounded_rectangle((3 * S, 3 * S, (w - 3) * S, (h - 3) * S), 9 * S, fill=CYAN + (90,))
    big.alpha_composite(glow.filter(ImageFilter.GaussianBlur(3 * S)))
    fill = lerp(GLASS, CYAN, 0.30 if pressed else 0.16)
    ImageDraw.Draw(big).rounded_rectangle((3 * S, 3 * S, (w - 3) * S - 1, (h - 3) * S - 1), 9 * S,
                                          fill=fill + (255,), outline=lerp(GLASS, CYAN, 0.65) + (255,),
                                          width=S)
    out = big.resize((w, h), Image.LANCZOS)
    flat = Image.new("RGB", (w, h), GLASS)  # Kicker tiles are opaque: on the taskbar colour
    flat.paste(out, (0, 0), out)
    return flat


def make_launcher(root, art):
    tiles = os.path.join(root, "opt/trinity/share/apps/kicker/tiles")
    for name, size in (("tiny", 24), ("normal", 42), ("large", 54)):
        for state in ("up", "down"):
            save(launcher_tile(size, state == "down"), os.path.join(tiles, f"syluxium_dusk_{name}_{state}.png"))
    pics = os.path.join(root, "opt/trinity/share/apps/kicker/pics")
    save(logo(art, "logo-hex.png", 32), os.path.join(pics, "syluxium_start.png"))


def avatar(size, letter=None):
    """Default user picture: a lavender circle with a simple person shape."""
    S = size * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    g.ellipse((0, 0, S - 1, S - 1), fill=LAVENDER + (255,), outline=lerp(LAVENDER, CYAN, 0.6) + (255,),
              width=max(1, S // 28))
    if letter:
        g.text((S / 2, S / 2), letter, font=font(int(S * 0.45)), fill=NIGHT + (255,), anchor="mm")
    else:
        g.ellipse((S * 0.35, S * 0.20, S * 0.65, S * 0.50), fill=NIGHT + (255,))
        g.pieslice((S * 0.18, S * 0.56, S * 0.82, S * 1.15), 180, 360, fill=NIGHT + (255,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, S - 1, S - 1), fill=255)
    im.putalpha(Image.composite(im.split()[3], Image.new("L", (S, S), 0), mask))
    return im.resize((size, size), Image.LANCZOS)


MENU_TOP_H = 64


def make_menu_top(root):
    pics = os.path.join(root, "opt/trinity/share/apps/kicker/pics")

    def strip(w):
        im = Image.new("RGB", (w, MENU_TOP_H), GLASS)
        g = ImageDraw.Draw(im)
        g.line((0, MENU_TOP_H - 1, w, MENU_TOP_H - 1), fill=lerp(GLASS, (255, 255, 255), 0.10))
        return im

    save(strip(16), os.path.join(pics, "syluxium_menu_top_tile.png"))
    left = strip(240)
    pic = avatar(44)
    left.paste(pic, (14, 10), pic)
    g = ImageDraw.Draw(left)
    g.text((70, 25), "Syluxium", font=font(15), fill=STAR, anchor="lm")
    g.text((70, 44), "Apps and settings", font=font(11, bold=False), fill=(170, 179, 214), anchor="lm")
    save(left, os.path.join(pics, "syluxium_menu_top_left.png"))
    save(strip(8), os.path.join(pics, "syluxium_menu_top_right.png"))


def make_taskbar(root):
    """Used when the taskbar's transparency is turned off: the glass colour
    with a cyan line along the top."""
    h = 54
    im = Image.new("RGB", (8, h), GLASS)
    g = ImageDraw.Draw(im)
    g.line((0, 0, 8, 0), fill=lerp(GLASS, CYAN, 0.40))
    save(im, os.path.join(root, "opt/trinity/share/apps/kicker/wallpapers/syluxium_taskbar.png"))


# ---------------------------------------------------------------------------
# Login screen, splash, shut-down pictures, wallpaper
# ---------------------------------------------------------------------------
def cover(im, w, h):
    s = max(w / im.width, h / im.height)
    im = im.resize((math.ceil(im.width * s), math.ceil(im.height * s)), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))


def dimmed(im, amount=0.45):
    return Image.blend(im.convert("RGB"), Image.new("RGB", im.size, (10, 12, 34)), amount)


def make_tdm_theme(root, art):
    d = os.path.join(root, "opt/trinity/share/apps/tdm/themes/syluxium")
    bg = Image.open(os.path.join(art, "login-background.jpg"))
    save(dimmed(cover(bg, 1920, 1080)), os.path.join(d, "background.jpg"), "JPEG", quality=90)
    save(logo(art, "logo-word.png", 180), os.path.join(d, "logo.png"))
    save(avatar(76), os.path.join(d, "avatar.png"))
    # sign-in arrow: cyan rounded square
    for state, k in (("normal", 0.0), ("prelight", 0.18)):
        S = 40 * SS
        im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        g = ImageDraw.Draw(im)
        g.rounded_rectangle((0, 0, S - 1, S - 1), 11 * SS, fill=lerp(CYAN, (255, 255, 255), k) + (255,))
        u = SS
        g.line((12 * u, 20 * u, 28 * u, 20 * u), fill=NIGHT + (255,), width=int(2.4 * u))
        g.line((21 * u, 13 * u, 28 * u, 20 * u, 21 * u, 27 * u), fill=NIGHT + (255,), width=int(2.4 * u),
               joint="curve")
        save(im.resize((40, 40), Image.LANCZOS), os.path.join(d, f"login_{state}.png"))
    # power button: pink ring with the power symbol
    for state, k in (("normal", 0.0), ("prelight", 0.25)):
        S = 48 * SS
        im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        g = ImageDraw.Draw(im)
        g.ellipse((0, 0, S - 1, S - 1), fill=lerp(GLASS, PINK, 0.12 + k) + (220,),
                  outline=lerp(GLASS, PINK, 0.55) + (255,), width=SS)
        u = SS
        g.arc((15 * u, 15 * u, 33 * u, 33 * u), 300, 240, fill=PINK + (255,), width=int(2.2 * u))
        g.line((24 * u, 12 * u, 24 * u, 23 * u), fill=PINK + (255,), width=int(2.2 * u))
        save(im.resize((48, 48), Image.LANCZOS), os.path.join(d, f"system_{state}.png"))
    save(dimmed(cover(bg, 320, 200)), os.path.join(d, "screenshot.png"))
    for old in ("background.png",):
        p = os.path.join(d, old)
        if os.path.exists(p):
            os.remove(p)


def splash_background(art, w, h):
    im = dimmed(cover(Image.open(os.path.join(art, "login-background.jpg")), w, h), 0.35).convert("RGBA")
    mark = logo(art, "logo-word.png", int(h * 0.28))
    im.alpha_composite(mark, ((w - mark.width) // 2, int(h * 0.30)))
    return im.convert("RGB")


def make_splash(root, art):
    d = os.path.join(root, "opt/trinity/share/apps/ksplash/Themes/Syluxium")
    save(splash_background(art, 1024, 768), os.path.join(d, "Background.png"))
    for w, h in ((1280, 800), (1280, 1024), (1366, 768), (1920, 1080)):
        save(splash_background(art, w, h), os.path.join(d, f"Background-{w}x{h}.png"))
    save(splash_background(art, 320, 240), os.path.join(d, "Preview.png"))


def shutdown_picture(art, w, h):
    im = dimmed(cover(Image.open(os.path.join(art, "login-background.jpg")), w, h), 0.35).convert("RGBA")
    S = 72 * SS
    sym = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(sym)
    g.ellipse((0, 0, S - 1, S - 1), fill=GLASS + (210,), outline=PINK + (255,), width=2 * SS)
    u = SS
    g.arc((20 * u, 20 * u, 52 * u, 52 * u), 300, 240, fill=PINK + (255,), width=4 * u)
    g.line((36 * u, 14 * u, 36 * u, 36 * u), fill=PINK + (255,), width=4 * u)
    sym = sym.resize((72, 72), Image.LANCZOS)
    im.alpha_composite(sym, ((w - 72) // 2, h // 2 - 56))
    mark = logo(art, "logo-hex.png", 32)
    im.alpha_composite(mark, (10, h - 42))
    return im


def make_wallpaper(root, art):
    im = Image.open(os.path.join(art, "wallpaper.jpg")).convert("RGB")
    save(im, os.path.join(root, "opt/trinity/share/wallpapers/syluxium-default.jpg"), "JPEG", quality=92)


def main(root):
    art = os.path.join(root, "usr/share/syluxium/dusk")
    make_window_theme(root)
    make_launcher(root, art)
    make_menu_top(root)
    make_taskbar(root)
    save(avatar(64), os.path.join(root, "opt/trinity/share/apps/tdm/faces/.default.face.icon"))
    make_splash(root, art)
    make_tdm_theme(root, art)
    pics = os.path.join(root, "opt/trinity/share/apps/ksmserver/pics")
    save(shutdown_picture(art, 188, 300), os.path.join(pics, "shutdownkonq.png"))
    save(shutdown_picture(art, 148, 217).convert("RGB"), os.path.join(pics, "shutdown.png"))
    make_wallpaper(root, art)
    print(f"Dusk artwork written under {root}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "rootfs")
