#!/usr/bin/env python3
"""Writes the Android launcher icons from one square master picture.

The macOS script (make_icon_assets.sh) builds these from the SVG with qlmanage and sips; this is the
same job on a machine without them, and it starts from a finished picture instead of the SVG.

  make_android_icons.py SRC.png [RES_ROOT]

SRC.png is the artwork: a square picture whose own edge is the icon (a badge, a rounded square, a
photo). It produces

  res/drawable-nodpi/ic_launcher_foreground.png   adaptive foreground, 432 px square, artwork in
                                                  the 66 of 108 dp safe zone, transparent outside
  res/mipmap-*/ic_launcher.png                    legacy icons, 48 to 192 px, corners flattened onto
                                                  white so the icon has no black wedges on a launcher
                                                  that does not mask legacy icons

RES_ROOT defaults to android/game/res. Needs Pillow (pip install pillow).
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
RES = os.path.join("android", "game", "res")

# Adaptive icons: 108 dp canvas, of which the launcher keeps the inner 66 dp at worst. The artwork is
# drawn at 66/108 of the canvas so no mask cuts it; the rest is the background layer's colour.
ADAPTIVE_PX = 432          # 108 dp at xxxhdpi
SAFE = 66.0 / 108.0

# Legacy icons, one per density. 48 dp is the launcher size.
LEGACY = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def load_square(path):
    """The picture, cropped to its centre square so nothing is stretched."""
    im = Image.open(path)
    im = im.convert("RGBA")
    w, h = im.size
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    return im.crop((left, top, left + side, top + side))


def foreground(src):
    """The artwork centred on a transparent 108 dp canvas, inside the safe zone."""
    art = src.resize((round(ADAPTIVE_PX * SAFE),) * 2, Image.LANCZOS)
    canvas = Image.new("RGBA", (ADAPTIVE_PX, ADAPTIVE_PX), (0, 0, 0, 0))
    off = (ADAPTIVE_PX - art.size[0]) // 2
    canvas.paste(art, (off, off))
    return canvas


def legacy(src, px):
    """A square icon: the picture on white, so the corners outside a rounded badge are not black."""
    flat = Image.new("RGB", src.size, (255, 255, 255))
    flat.paste(src, (0, 0), src)
    return flat.resize((px, px), Image.LANCZOS)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    src_path = argv[1]
    res = argv[2] if len(argv) > 2 else os.path.join(REPO, RES)
    src = load_square(src_path)

    out = os.path.join(res, "drawable-nodpi", "ic_launcher_foreground.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    foreground(src).save(out, optimize=True)
    print("wrote", os.path.relpath(out, REPO), ADAPTIVE_PX, "px")

    for density, px in LEGACY.items():
        out = os.path.join(res, "mipmap-" + density, "ic_launcher.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        legacy(src, px).save(out, optimize=True)
        print("wrote", os.path.relpath(out, REPO), px, "px")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
