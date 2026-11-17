#!/usr/bin/env python3
"""Draw Syluxium's green "Start" button tiles for TDE's Kicker (original
artwork, XP/7-era look: saturated green, glossy top half, darker edge).
Writes <out>/syluxium_green_{tiny,normal,large}_{up,down}.png."""
import os
import sys

from PIL import Image


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def tile(size: int, pressed: bool) -> Image.Image:
    im = Image.new("RGB", (size, size))
    px = im.load()
    top, mid, bottom = ((96, 196, 96), (46, 160, 52), (24, 112, 30))
    if pressed:  # darker, gloss reversed
        top, mid, bottom = ((30, 120, 36), (40, 140, 46), (70, 170, 74))
    for y in range(size):
        t = y / (size - 1)
        c = lerp(top, mid, t / 0.45) if t < 0.45 else lerp(mid, bottom, (t - 0.45) / 0.55)
        if not pressed and t < 0.45:  # gloss: lighten the upper half
            gloss = 0.35 * (1 - t / 0.45)
            c = lerp(c, (255, 255, 255), gloss)
        for x in range(size):
            px[x, y] = c
    return im


def main(out: str) -> None:
    os.makedirs(out, exist_ok=True)
    for name, size in (("tiny", 24), ("normal", 42), ("large", 54)):
        for state in ("up", "down"):
            tile(size, state == "down").save(os.path.join(out, f"syluxium_green_{name}_{state}.png"))
    print(f"tiles written to {out}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "rootfs/opt/trinity/share/apps/kicker/tiles")
