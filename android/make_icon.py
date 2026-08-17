#!/usr/bin/env python3
"""Generate the Android launcher icon at every density.

The SDL Android template ships SDL's own logo as ic_launcher.png. That is
SDL's mark, not this project's, so it must not be published as the icon of a
different app -- this draws an original one instead.

    python3 android/make_icon.py

Writes app/src/main/res/mipmap-*/ic_launcher.png.
"""
import os

from PIL import Image, ImageDraw

# density bucket -> edge length in px
DENSITIES = {
    "mdpi": 48,
    "hdpi": 72,
    "xhdpi": 96,
    "xxhdpi": 144,
    "xxxhdpi": 192,
}

BG_TOP = (32, 36, 48)
BG_BOTTOM = (14, 15, 19)
STREAKS = [(255, 138, 30), (255, 176, 62), (250, 96, 24)]

SS = 8  # supersample factor; the icon is small and the diagonals alias badly


def render(size):
    n = size * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))

    # vertical gradient body
    grad = Image.new("RGBA", (1, n))
    gp = grad.load()
    for y in range(n):
        t = y / max(1, n - 1)
        gp[0, y] = (
            int(BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t),
            int(BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t),
            int(BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t),
            255,
        )
    grad = grad.resize((n, n))

    # rounded-square mask
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, n - 1, n - 1], radius=int(n * 0.22), fill=255)
    img.paste(grad, (0, 0), mask)

    # three swept speed streaks, thick to thin, leaning forward
    d = ImageDraw.Draw(img)
    for i, colour in enumerate(STREAKS):
        y = n * (0.30 + 0.20 * i)
        w = n * (0.115 - 0.022 * i)
        x0 = n * (0.14 + 0.10 * i)
        x1 = n * (0.86 - 0.04 * i)
        lean = n * 0.11
        d.polygon(
            [(x0, y + w / 2), (x0 + lean, y - w / 2),
             (x1, y - w / 2), (x1 - lean, y + w / 2)],
            fill=colour + (255,))

    out = img.resize((size, size), Image.LANCZOS)
    # keep the transparent corners crisp after the downsample
    small_mask = mask.resize((size, size), Image.LANCZOS)
    out.putalpha(small_mask)
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for bucket, size in DENSITIES.items():
        d = os.path.join(here, "app", "src", "main", "res", "mipmap-" + bucket)
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, "ic_launcher.png")
        render(size).save(p)
        print("  %-28s %dx%d" % (os.path.relpath(p, here), size, size))


if __name__ == "__main__":
    main()
