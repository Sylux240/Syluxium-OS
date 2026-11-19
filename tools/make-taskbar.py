#!/usr/bin/env python3
"""Draw Syluxium's blue XP-era taskbar background for TDE's Kicker (original
artwork: a vertical blue gradient with a light top edge and a glossy upper
part). Writes <out>/syluxium_taskbar.png (tiled horizontally by Kicker)."""
import sys

from PIL import Image


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def main(out: str) -> None:
    w, h = 64, 30
    im = Image.new("RGB", (w, h))
    px = im.load()
    stops = [(0.0, (70, 132, 230)), (0.12, (52, 112, 222)), (0.5, (36, 92, 214)),
             (0.85, (28, 80, 200)), (1.0, (22, 66, 176))]
    for y in range(h):
        t = y / (h - 1)
        for i in range(len(stops) - 1):
            if stops[i][0] <= t <= stops[i + 1][0]:
                a, b = stops[i], stops[i + 1]
                c = lerp(a[1], b[1], (t - a[0]) / (b[0] - a[0]))
                break
        if y == 0:
            c = (120, 170, 245)  # light top edge
        for x in range(w):
            px[x, y] = c
    im.save(f"{out}/syluxium_taskbar.png")


if __name__ == "__main__":
    main(sys.argv[1])
