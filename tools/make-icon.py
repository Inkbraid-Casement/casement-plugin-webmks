#!/usr/bin/env python3
"""Draws icon.png: a monitor with a console prompt in front of a second
screen, for "the console of a virtual machine". 256x256, transparent.

    python3 tools/make-icon.py icon.png
"""

import sys

from PIL import Image, ImageDraw

SIZE = 256
SCALE = 4  # drawn larger and scaled down, for smooth edges


def px(v):
    return int(round(v * SCALE))


def box(x0, y0, x1, y1):
    return [px(x0), px(y0), px(x1), px(y1)]


def main(path):
    img = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # The second machine behind, up and to the right.
    d.rounded_rectangle(box(70, 26, 238, 142), radius=px(14), fill=(96, 125, 139, 255))
    d.rounded_rectangle(box(82, 38, 226, 130), radius=px(6), fill=(176, 190, 197, 255))

    # The monitor in front.
    d.rounded_rectangle(box(18, 66, 206, 196), radius=px(16), fill=(38, 50, 56, 255))
    d.rounded_rectangle(box(30, 78, 194, 184), radius=px(7), fill=(21, 101, 192, 255))
    # Stand.
    d.polygon([(px(96), px(196)), (px(128), px(196)), (px(136), px(222)), (px(88), px(222))],
              fill=(38, 50, 56, 255))
    d.rounded_rectangle(box(64, 218, 160, 232), radius=px(6), fill=(38, 50, 56, 255))

    # A prompt on the screen: ">" and a cursor block.
    w = px(11)
    d.line([(px(52), px(104)), (px(80), px(130)), (px(52), px(156))], fill=(255, 255, 255, 255),
           width=w, joint="curve")
    for x, y in ((52, 104), (52, 156)):
        d.ellipse(box(x - 5.5, y - 5.5, x + 5.5, y + 5.5), fill=(255, 255, 255, 255))
    d.rounded_rectangle(box(94, 146, 140, 160), radius=px(3), fill=(129, 212, 250, 255))

    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save(path, optimize=True)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "icon.png")
