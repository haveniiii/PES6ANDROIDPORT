#!/usr/bin/env python3
"""The APK's launcher icon (docs/14): an adaptive icon (Android 8+, our minimum) whose
foreground is the user's logo, its diagonal inside the 66 dp safe circle of the 108 dp layer, on a
white background, so every launcher mask (circle, squircle, square) keeps all of it.

A finished square icon instead (art to the edges, dark rounded corners: what icon generators
give) fills the 72 dp the masks show, cut inside its corners, over itself blurred out to the
108 dp layer's edge.

    tools/app_icon.py <logo image> <res dir>
Writes <res>/mipmap-anydpi-v26/ic_launcher.xml and <res>/mipmap-<dpi>/ic_launcher_fg.png.
The logo is the user's own asset (mods/app_icon/, not versioned); without it,
tools/android_build.sh passes no image and a plain icon is drawn instead."""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

DPI = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}
LAYER_DP, SAFE_DP, SHOWN_DP = 108, 66, 72


def full_icon(img):
    """A finished icon: all four corners (nearly) black, its art filling the square."""
    w, h = img.size
    corners = [img.getpixel((x, y)) for x in (1, w - 2) for y in (1, h - 2)]
    return all(sum(c) < 60 for c in corners)


def inside_corners(img):
    """The largest centred square clear of the dark rounded corners: walk the diagonal in
    from a corner until the pixel is no longer dark, then a little more for the rim."""
    w, h = img.size
    d = 0
    while d < w // 3 and sum(img.getpixel((d, d))) < 60:
        d += 1
    d += w // 100
    return img.crop((d, d, w - d, h - d))


def logo(path):
    """The logo cropped to what is not (nearly) white."""
    img = Image.open(path).convert("RGB")
    grey = img.convert("L").point(lambda v: 255 if v < 235 else 0)
    box = grey.getbbox() or (0, 0) + img.size
    return img.crop(box)


def plain():
    """No logo: a plain gold disc, just so the APK has an icon."""
    img = Image.new("RGB", (256, 256), "white")
    ImageDraw.Draw(img).ellipse((24, 24, 232, 232), fill=(222, 170, 20))
    return img


def main():
    src, res = (sys.argv[1] if len(sys.argv) > 2 else None), sys.argv[-1]
    img = Image.open(src).convert("RGB") if src else None
    if img and full_icon(img):
        art = inside_corners(img)
        for dpi, k in DPI.items():
            size, shown = round(LAYER_DP * k), round(SHOWN_DP * k * 1.04)
            layer = art.resize((size, size), Image.LANCZOS).filter(ImageFilter.GaussianBlur(size / 25))
            fit = art.resize((shown, shown), Image.LANCZOS)
            layer.paste(fit, ((size - shown) // 2, (size - shown) // 2))
            os.makedirs(os.path.join(res, "mipmap-" + dpi), exist_ok=True)
            layer.save(os.path.join(res, "mipmap-" + dpi, "ic_launcher_fg.png"))
        write_xml(res)
        return
    art = logo(src) if src else plain()
    for dpi, k in DPI.items():
        size, safe = round(LAYER_DP * k), SAFE_DP * k * 0.96
        s = safe / (art.width ** 2 + art.height ** 2) ** 0.5   # whole logo inside the safe circle
        fit = art.resize((round(art.width * s), round(art.height * s)), Image.LANCZOS)
        layer = Image.new("RGB", (size, size), "white")
        layer.paste(fit, ((size - fit.width) // 2, (size - fit.height) // 2))
        os.makedirs(os.path.join(res, "mipmap-" + dpi), exist_ok=True)
        layer.save(os.path.join(res, "mipmap-" + dpi, "ic_launcher_fg.png"))
    write_xml(res)


def write_xml(res):
    os.makedirs(os.path.join(res, "mipmap-anydpi-v26"), exist_ok=True)
    with open(os.path.join(res, "mipmap-anydpi-v26", "ic_launcher.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@android:color/white" />\n'
                '    <foreground android:drawable="@mipmap/ic_launcher_fg" />\n'
                '</adaptive-icon>\n')


if __name__ == "__main__":
    main()
