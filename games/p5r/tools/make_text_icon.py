# A plain launcher icon with no game art: "P5R" in white on red, in the same files and sizes as make_icon.py (the
# public build uses it; make_icon.py makes one from a picture of your own).
#   python games/p5r/tools/make_text_icon.py [<icon dir>]   (default games/p5r/icon)
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "icon")
RED, WHITE = (196, 18, 28), (255, 255, 255)
FONTS = ["C:/Windows/Fonts/arialbd.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
         "/System/Library/Fonts/Supplemental/Arial Bold.ttf"]


def font(px):
    for f in FONTS:
        if os.path.exists(f):
            return ImageFont.truetype(f, px)
    return ImageFont.load_default(px)


def square(size, text_share):
    # The text's width as a share of the square, centred.
    im = Image.new("RGB", (size, size), RED)
    d = ImageDraw.Draw(im)
    f = font(size)
    box = d.textbbox((0, 0), "P5R", font=f)
    f = font(round(size * size * text_share / (box[2] - box[0])))
    box = d.textbbox((0, 0), "P5R", font=f)
    d.text(((size - (box[2] - box[0])) / 2 - box[0], (size - (box[3] - box[1])) / 2 - box[1]), "P5R", font=f, fill=WHITE)
    return im


def main():
    os.makedirs(OUT, exist_ok=True)
    square(1024, 0.7).save(os.path.join(OUT, "icon.png"))
    res = os.path.join(OUT, "res")
    # Legacy square icons, and the adaptive layer (108 dp, of which every launcher shows the middle 72 dp or less).
    for folder, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(res, "mipmap-" + folder)
        os.makedirs(d, exist_ok=True)
        square(px * 4, 0.7).resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
        layer = round(px * 108 / 48)
        square(layer * 4, 0.46).resize((layer, layer), Image.LANCZOS).save(os.path.join(d, "ic_launcher_layer.png"))
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
