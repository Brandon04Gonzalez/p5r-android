# The launcher icon from a picture of the mask (white on black): python games/p5r/tools/make_icon.py <picture>
# Cleans the edges (thresholded at 4x, then smoothed), centres the mask on a black square inside the part of an
# adaptive icon every launcher mask shows, and writes games/p5r/icon/icon.png (1024 px) and the launcher sizes under
# games/p5r/icon/res (mipmap-*/ic_launcher.png, the adaptive icon's layer and its xml), which build_apk.ps1 uses.
import os
import sys

from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "icon")


def mask_of(path):
    grey = Image.open(path).convert("L")
    big = grey.resize((grey.width * 4, grey.height * 4), Image.LANCZOS)
    hard = big.point(lambda v: 255 if v > 128 else 0)
    soft = hard.filter(ImageFilter.GaussianBlur(2.5))  # antialiased edges after the threshold
    box = soft.point(lambda v: 255 if v > 8 else 0).getbbox()  # the mask itself, without the black around it
    return soft.crop(box)


def square(mask, size, width_share):
    # The mask's width as a share of the square, centred (a little above the middle: the mask's chin is pointed).
    w = round(size * width_share)
    h = round(mask.height * w / mask.width)
    canvas = Image.new("L", (size, size), 0)
    canvas.paste(mask.resize((w, h), Image.LANCZOS), ((size - w) // 2, (size - h) // 2 - size // 40))
    return Image.merge("RGB", (canvas, canvas, canvas))


def main():
    mask = mask_of(sys.argv[1])
    os.makedirs(OUT, exist_ok=True)
    square(mask, 1024, 0.78).save(os.path.join(OUT, "icon.png"))
    res = os.path.join(OUT, "res")
    # Legacy square icons (the mask wide), and the adaptive layer (108 dp): every mask shows the middle 72 dp and a
    # round one less, so the mask spans 56% of the layer there.
    for folder, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(res, "mipmap-" + folder)
        os.makedirs(d, exist_ok=True)
        square(mask, px * 4, 0.78).resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
        layer = round(px * 108 / 48)
        square(mask, layer * 4, 0.56).resize((layer, layer), Image.LANCZOS).save(os.path.join(d, "ic_launcher_layer.png"))
    d = os.path.join(res, "mipmap-anydpi-v26")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "ic_launcher.xml"), "w", encoding="utf-8") as x:
        x.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@mipmap/ic_launcher_layer" />\n'
                '    <foreground android:drawable="@android:color/transparent" />\n'
                '</adaptive-icon>\n')
    print("icon: %s, launcher sizes in %s" % (os.path.join(OUT, "icon.png"), res))


if __name__ == "__main__":
    main()
