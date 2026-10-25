#!/usr/bin/env python3
"""Build a stand-in for the Switch shared font type 5 (Nintendo extension: controller button icons).

The console's own extension font lives in a firmware system archive we cannot (and must not) use, so this script
draws its own simple button icons and adds them to a host TrueType font:

    make_button_font.py [--base HOST.ttf] [--out OUT.ttf] [--preview PNG]

The default base is macOS's Arial Unicode and the default output is private/wonder/fonts/shared_5.ttf, which is
where runtime/src/hle_pl.cpp looks first for type 5 (<data root>/fonts/shared_<type>.ttf). The output embeds the host
font, so it is for local use only and must never be committed.

Only the letters inside the icons (A, B, X, Y, L, R, Z, S) come from the host font; everything else is drawn here.
The codepoints are the ones Super Mario Bros. Wonder's messages (Mals/*/Key.msbt, ControlGuide*.msbt) and layouts
use; the tags in its messages (e.g. the title's "Press [L] + [R] to Start") resolve to Key.msbt's entries.

Requires fontTools (and Pillow for --preview).
"""
import argparse
import math
import os
import sys

from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_BASE = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
DEFAULT_OUT = os.path.join(REPO, "private", "wonder", "fonts", "shared_5.ttf")

K = 0.5522847498  # cubic Bezier circle constant

# codepoint -> (glyph name, kind, argument)
ICONS = {
    # face buttons (the E0Ex set is what Wonder uses; E0Ax is the older set with the same meaning)
    0xE0E0: ("btn_A", "circle_letter", "A"),
    0xE0E1: ("btn_B", "circle_letter", "B"),
    0xE0E2: ("btn_X", "circle_letter", "X"),
    0xE0E3: ("btn_Y", "circle_letter", "Y"),
    0xE0E4: ("btn_L", "shoulder", "L"),
    0xE0E5: ("btn_R", "shoulder", "R"),
    0xE0E6: ("btn_ZL", "trigger", "ZL"),
    0xE0E7: ("btn_ZR", "trigger", "ZR"),
    0xE0E8: ("btn_SL", "rail", "SL"),
    0xE0E9: ("btn_SR", "rail", "SR"),
    0xE0A0: ("btn_A_alt", "circle_letter", "A"),
    0xE0A1: ("btn_B_alt", "circle_letter", "B"),
    0xE0A2: ("btn_X_alt", "circle_letter", "X"),
    0xE0A3: ("btn_Y_alt", "circle_letter", "Y"),
    0xE0A4: ("btn_L_alt", "shoulder", "L"),
    0xE0A5: ("btn_R_alt", "shoulder", "R"),
    0xE0A6: ("btn_ZL_alt", "trigger", "ZL"),
    0xE0A7: ("btn_ZR_alt", "trigger", "ZR"),
    0xE0A8: ("btn_SL_alt", "rail", "SL"),
    0xE0A9: ("btn_SR_alt", "rail", "SR"),
    0xE0EF: ("btn_plus", "circle_sign", "+"),
    0xE0F0: ("btn_minus", "circle_sign", "-"),
    # directional pad: one arm highlighted, or the whole pad
    0xE0AB: ("dpad_right", "dpad", "right"),
    0xE0AC: ("dpad_down", "dpad", "down"),
    0xE0AD: ("dpad_up", "dpad", "up"),
    0xE0AE: ("dpad_left", "dpad", "left"),
    0xE0EA: ("dpad_all", "dpad", "all"),
    # sticks
    0xE100: ("stick", "stick", ""),
    0xE101: ("stick_L", "stick", "L"),
    # tilt directions (follow a stick icon: "[stick][down] to crouch")
    0xE090: ("arrow_right", "arrow", 0),
    0xE091: ("arrow_left", "arrow", 180),
    0xE092: ("arrow_up", "arrow", 90),
    0xE093: ("arrow_down", "arrow", 270),
    0xE094: ("arrow_rightup", "arrow", 45),
    0xE095: ("arrow_rightdown", "arrow", 315),
    0xE096: ("arrow_leftdown", "arrow", 225),
    0xE097: ("arrow_leftup", "arrow", 135),
    # small triangles around the stick icon in menus ("<[stick]>")
    0xE145: ("tri_left", "triangle", 180),
    0xE146: ("tri_right", "triangle", 0),
}


class Builder:
    def __init__(self, font):
        self.font = font
        self.upm = font["head"].unitsPerEm
        self.cmap = font.getBestCmap()
        self.glyf = font["glyf"]
        self.gs = font.getGlyphSet()
        cap = self._bounds(self.cmap[ord("H")])
        self.cap = cap[3] if cap else 0.716 * self.upm
        # icon box: a bit taller than the capitals, dipping slightly below the baseline
        pad = 0.06 * self.upm
        self.bottom = -pad
        self.top = self.cap + pad
        self.size = self.top - self.bottom
        self.cy = (self.top + self.bottom) / 2
        self.side = 0.07 * self.upm  # side bearing

    def _bounds(self, gname):
        bp = BoundsPen(self.gs)
        self.gs[gname].draw(bp)
        return bp.bounds

    # --- primitive shapes (drawn clockwise = filled in TrueType; hole=True cuts one out) ---

    def circle(self, pen, cx, cy, r, hole=False):
        if hole:
            pen = ReverseContourPen(pen)
        k = K * r
        pen.moveTo((cx + r, cy))
        pen.curveTo((cx + r, cy - k), (cx + k, cy - r), (cx, cy - r))
        pen.curveTo((cx - k, cy - r), (cx - r, cy - k), (cx - r, cy))
        pen.curveTo((cx - r, cy + k), (cx - k, cy + r), (cx, cy + r))
        pen.curveTo((cx + k, cy + r), (cx + r, cy + k), (cx + r, cy))
        pen.closePath()

    def rrect(self, pen, x0, y0, x1, y1, r, hole=False):
        if hole:
            pen = ReverseContourPen(pen)
        r = min(r, (x1 - x0) / 2, (y1 - y0) / 2)
        k = K * r
        pen.moveTo((x0, y0 + r))
        pen.lineTo((x0, y1 - r))
        pen.curveTo((x0, y1 - r + k), (x0 + r - k, y1), (x0 + r, y1))
        pen.lineTo((x1 - r, y1))
        pen.curveTo((x1 - r + k, y1), (x1, y1 - r + k), (x1, y1 - r))
        pen.lineTo((x1, y0 + r))
        pen.curveTo((x1, y0 + r - k), (x1 - r + k, y0), (x1 - r, y0))
        pen.lineTo((x0 + r, y0))
        pen.curveTo((x0 + r - k, y0), (x0, y0 + r - k), (x0, y0 + r))
        pen.closePath()

    def poly(self, pen, pts, hole=False):
        """pts go clockwise (filled); hole=True cuts them out instead."""
        if hole:
            pen = ReverseContourPen(pen)
        pen.moveTo(pts[0])
        for p in pts[1:]:
            pen.lineTo(p)
        pen.closePath()

    def text(self, pen, s, cx, cy, cap_h, hole=True):
        """Draw the host font's outlines for s, centred on (cx, cy), scaled so capitals are cap_h tall."""
        scale = cap_h / self.cap
        track = -0.02 * self.upm
        names = [self.cmap[ord(ch)] for ch in s]
        widths = [self.font["hmtx"][n][0] for n in names]
        total = sum(widths) + track * (len(s) - 1)
        x = -total / 2
        for n, w in zip(names, widths):
            t = (scale, 0, 0, scale, cx + x * scale, cy - self.cap * scale / 2)
            target = ReverseContourPen(pen) if hole else pen
            self.gs[n].draw(TransformPen(target, t))
            x += w + track

    def text_width(self, s, cap_h):
        scale = cap_h / self.cap
        return scale * (sum(self.font["hmtx"][self.cmap[ord(ch)]][0] for ch in s) - 0.02 * self.upm * (len(s) - 1))

    # --- icons ---

    def build(self, kind, arg):
        """Returns (TTGlyph, advance)."""
        tt = TTGlyphPen(None)
        pen = Cu2QuPen(tt, max_err=1.0, reverse_direction=False)
        S, cy, side = self.size, self.cy, self.side
        if kind == "circle_letter":
            r = S / 2
            cx = side + r
            self.circle(pen, cx, cy, r)
            self.text(pen, arg, cx, cy, 0.58 * S)
            adv = 2 * r + 2 * side
        elif kind == "circle_sign":
            r = S / 2
            cx = side + r
            self.circle(pen, cx, cy, r)
            a, t = 0.30 * S, 0.075 * S  # half arm length, half thickness
            if arg == "+":
                pts = [(cx - t, cy + a), (cx + t, cy + a), (cx + t, cy + t), (cx + a, cy + t), (cx + a, cy - t),
                       (cx + t, cy - t), (cx + t, cy - a), (cx - t, cy - a), (cx - t, cy - t), (cx - a, cy - t),
                       (cx - a, cy + t), (cx - t, cy + t)]
            else:
                pts = [(cx - a, cy + t), (cx + a, cy + t), (cx + a, cy - t), (cx - a, cy - t)]
            self.poly(pen, pts, hole=True)
            adv = 2 * r + 2 * side
        elif kind in ("shoulder", "trigger", "rail"):
            h = {"shoulder": 0.78, "trigger": 0.78, "rail": 0.62}[kind] * S
            cap_h = 0.50 * S if kind != "rail" else 0.40 * S
            tw = self.text_width(arg, cap_h)
            w = max(tw + 0.42 * S, {"shoulder": 1.2, "trigger": 1.45, "rail": 1.2}[kind] * S)
            x0, y0 = side, cy - h / 2
            x1, y1 = side + w, cy + h / 2
            if kind == "shoulder":
                # shoulder-button shape: rounded bottom corners small, top corners large
                self.rrect(pen, x0, y0, x1, y1, 0.34 * h)
            elif kind == "trigger":
                self.rrect(pen, x0, y0, x1, y1, 0.18 * h)
            else:
                self.rrect(pen, x0, y0, x1, y1, 0.5 * h)
            self.text(pen, arg, (x0 + x1) / 2, cy, cap_h)
            adv = w + 2 * side
        elif kind == "dpad":
            cx = side + S / 2
            a = S / 2           # arm reach
            t = 0.18 * S        # half arm width
            ring = 0.07 * S     # outline thickness

            def cross(a, t):
                return [(cx - t, cy + a), (cx + t, cy + a), (cx + t, cy + t), (cx + a, cy + t), (cx + a, cy - t),
                        (cx + t, cy - t), (cx + t, cy - a), (cx - t, cy - a), (cx - t, cy - t), (cx - a, cy - t),
                        (cx - a, cy + t), (cx - t, cy + t)]

            self.poly(pen, cross(a, t))
            if arg != "all":
                self.poly(pen, cross(a - ring, t - ring), hole=True)
                # filled arm, a little inset from the outline
                ang = {"right": 0, "up": 90, "left": 180, "down": 270}[arg]
                arm = [(t * 0.3, t - ring), (a - ring, t - ring), (a - ring, -(t - ring)), (t * 0.3, -(t - ring))]
                self.poly(pen, self._rot(arm, ang, cx, cy))
            else:
                # a small hole in the middle so the pad reads as a pad and not a plus sign
                self.circle(pen, cx, cy, 0.09 * S, hole=True)
            adv = S + 2 * side
        elif kind == "stick":
            r = S / 2
            cx = side + r
            self.circle(pen, cx, cy, r)
            self.circle(pen, cx, cy, r - 0.08 * S, hole=True)
            ri = 0.33 * S
            self.circle(pen, cx, cy, ri)
            if arg:
                self.text(pen, arg, cx, cy, 0.36 * S)
            adv = 2 * r + 2 * side
        elif kind == "arrow":
            # arrow pointing right, rotated; drawn in a square box a bit smaller than the icons
            L = 0.80 * S
            cx = side + L / 2
            hl, hw, sw = 0.45 * L, 0.42 * L, 0.13 * L  # head length, head half width, shaft half width
            pts = [(-L / 2, sw), (L / 2 - hl, sw), (L / 2 - hl, hw), (L / 2, 0), (L / 2 - hl, -hw),
                   (L / 2 - hl, -sw), (-L / 2, -sw)]
            self.poly(pen, self._rot(pts, arg, cx, cy))
            adv = L + 2 * side
        elif kind == "triangle":
            L = 0.45 * S
            cx = side + L / 2
            pts = [(-L / 2, 0.55 * L), (L / 2, 0), (-L / 2, -0.55 * L)]  # clockwise, pointing right
            self.poly(pen, self._rot(pts, arg, cx, cy))
            adv = L + 2 * side
        else:
            raise ValueError(kind)
        return tt.glyph(), int(round(adv))

    @staticmethod
    def _rot(pts, deg, cx, cy):
        c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        return [(cx + x * c - y * s, cy + x * s + y * c) for x, y in pts]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", default=DEFAULT_BASE, help="host TrueType font (glyf outlines)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output TTF (local use only, never commit)")
    ap.add_argument("--preview", help="also render the new glyphs to this PNG (needs Pillow)")
    args = ap.parse_args()

    font = TTFont(args.base)
    if "glyf" not in font:
        sys.exit("base font must have TrueType (glyf) outlines")
    b = Builder(font)
    order = font.getGlyphOrder()
    hmtx = font["hmtx"]
    new = []
    for cp, (name, kind, arg) in ICONS.items():
        glyph, adv = b.build(kind, arg)
        if name in font["glyf"].glyphs:
            name += ".btn"
        order.append(name)
        font["glyf"][name] = glyph
        glyph.recalcBounds(font["glyf"])
        hmtx[name] = (adv, glyph.xMin if hasattr(glyph, "xMin") else 0)
        new.append((cp, name))
    font.setGlyphOrder(order)
    font["glyf"].glyphOrder = order
    if "post" in font and font["post"].formatType == 2.0:
        font["post"].extraNames = []
        font["post"].mapping = {}
    for table in font["cmap"].tables:
        if table.isUnicode():
            if table.format == 4 or table.format == 12:
                for cp, name in new:
                    table.cmap[cp] = name
    # tables tied to the old glyph count / checksums: drop them, the rest is recomputed on save
    for tag in ("hdmx", "LTSH", "DSIG", "VDMX"):
        if tag in font:
            del font[tag]
    font["maxp"].numGlyphs = len(order)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    font.save(args.out)
    print(f"wrote {args.out}: {len(new)} button glyphs added to {os.path.basename(args.base)}")

    if args.preview:
        render_preview(args.out, [cp for cp, _ in new], args.preview)


def render_preview(path, cps, png):
    from PIL import Image, ImageDraw, ImageFont

    big = ImageFont.truetype(path, 72)
    small = ImageFont.truetype(path, 20)
    cols = 8
    cw, ch = 170, 150
    rows = (len(cps) + cols - 1) // cols
    img = Image.new("RGB", (cols * cw, rows * ch + 90), "white")
    d = ImageDraw.Draw(img)
    for i, cp in enumerate(cps):
        x, y = (i % cols) * cw, (i // cols) * ch
        d.line([(x + 10, y + 100), (x + cw - 10, y + 100)], fill=(200, 200, 255))  # baseline guide
        d.text((x + 20, y + 100), "H" + chr(cp), font=big, fill="black", anchor="ls")
        d.text((x + 10, y + 115), f"U+{cp:04X} {ICONS[cp][0]}", font=small, fill=(90, 90, 90))
    d.text((10, rows * ch + 25), "Press   +   to Start    / Press    ",
           font=ImageFont.truetype(path, 40), fill="black")
    img.save(png)
    print(f"preview {png}")


if __name__ == "__main__":
    main()
