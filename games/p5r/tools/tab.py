# Drive the game on the Android device from the PC, for testing: screenshots (scaled down), controller buttons and
# touches. The game takes controller keys from `input keyevent` (platform_android_input.cpp maps them as a pad).
#   python tab.py shot NAME [--w 960]         -> private/p5r/shots/NAME.png (landscape, scaled)
#   python tab.py key A|B|X|Y|L|R|ZL|ZR|PLUS|MINUS|UP|DOWN|LEFT|RIGHT [--ms 120] [--n 1] [--gap 0.4]
#   python tab.py tap X Y                     (0..1 across the landscape screen)
#   python tab.py touch A [--ms 120]          (the drawn touch button, P5R layout of runtime/src/touch_layout.cpp)
#   python tab.py stick DX DY --ms 1500       (left stick: a finger in the stick area held for --ms; rstick: camera)
#   python tab.py seq "A;wait 1;DOWN*3;A"     (several keys, `wait s` between them; KEY*n repeats)
import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
ADB = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk", "platform-tools", "adb.exe")
SHOTS = os.path.join(ROOT, "private", "p5r", "shots")

# Nintendo button -> Android keycode, the inverse of key_to_button. A and B go as Enter and Escape: the Lenovo tablet
# takes BUTTON_B (the right face button = A) for its stylus screenshot tool.
KEYS = {
    "A": "KEYCODE_ENTER", "B": "KEYCODE_ESCAPE", "X": "KEYCODE_BUTTON_Y", "Y": "KEYCODE_BUTTON_X",
    "L": "KEYCODE_BUTTON_L1", "R": "KEYCODE_BUTTON_R1", "ZL": "KEYCODE_BUTTON_L2", "ZR": "KEYCODE_BUTTON_R2",
    "PLUS": "KEYCODE_BUTTON_START", "MINUS": "KEYCODE_BUTTON_SELECT", "UP": "KEYCODE_DPAD_UP",
    "DOWN": "KEYCODE_DPAD_DOWN", "LEFT": "KEYCODE_DPAD_LEFT", "RIGHT": "KEYCODE_DPAD_RIGHT",
    "LS": "KEYCODE_BUTTON_THUMBL", "RS": "KEYCODE_BUTTON_THUMBR",
}


# The P5R touch layout (x, y across the landscape screen), as touch_layout.cpp kP5R.
TOUCH = {
    "A": (0.940, 0.66), "B": (0.875, 0.80), "X": (0.875, 0.52), "Y": (0.810, 0.66), "R": (0.945, 0.08),
    "ZR": (0.945, 0.24), "L": (0.055, 0.08), "ZL": (0.055, 0.24), "MINUS": (0.440, 0.07), "PLUS": (0.560, 0.07),
    "UP": (0.135, 0.37), "DOWN": (0.135, 0.55), "LEFT": (0.085, 0.46), "RIGHT": (0.185, 0.46),
}


def adb(*args, out=False):
    cmd = [ADB] + list(args)
    if out:
        return subprocess.run(cmd, capture_output=True).stdout
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def screen():
    # The physical size is portrait (1200x1920); the game runs landscape.
    s = adb("shell", "wm", "size", out=True).decode().split(":")[-1].strip()
    w, h = (int(v) for v in s.split("x"))
    return max(w, h), min(w, h)


def shot(name, width):
    from io import BytesIO
    from PIL import Image
    png = adb("exec-out", "screencap", "-p", out=True)
    im = Image.open(BytesIO(png)).convert("RGB")
    if im.height > im.width:
        im = im.rotate(90, expand=True)
    im = im.resize((width, int(im.height * width / im.width)), Image.BILINEAR)
    os.makedirs(SHOTS, exist_ok=True)
    path = os.path.join(SHOTS, name + ".png")
    im.save(path)
    print(path)


def key(name, ms=120):
    code = KEYS[name.upper()]
    src = "keyboard" if code in ("KEYCODE_ENTER", "KEYCODE_ESCAPE") else "gamepad"
    adb("shell", "input", src, "keyevent", "--duration", str(ms), code)


def touch(name, ms=120):
    w, h = screen()
    x, y = TOUCH[name.upper()]
    px, py = str(int(x * w)), str(int(y * h))
    adb("shell", "input", "swipe", px, py, px, py, str(ms))


def tap(x, y):
    w, h = screen()
    adb("shell", "input", "tap", str(int(x * w)), str(int(y * h)))


def stick(dx, dy, ms, right=False):
    # One finger held at the deflection (dx, dy: -1..1, y up) for ms, all on the device so the timing holds.
    w, h = screen()
    x0, y0 = (int(0.70 * w), int(0.40 * h)) if right else (int(0.18 * w), int(0.80 * h))
    r = 0.12 * h
    x1, y1 = int(x0 + dx * r * 1.1), int(y0 - dy * r * 1.1)
    xm, ym = (x0 + x1) // 2, (y0 + y1) // 2
    adb("shell", f"input motionevent DOWN {x0} {y0}; input motionevent MOVE {xm} {ym}; "
                 f"input motionevent MOVE {x1} {y1}; sleep {ms / 1000:.2f}; input motionevent UP {x1} {y1}")


def seq(text, gap):
    for item in [s.strip() for s in text.split(";") if s.strip()]:
        if item.startswith("wait"):
            time.sleep(float(item.split()[1]))
            continue
        name, _, n = item.partition("*")
        for _ in range(int(n or 1)):
            key(name)
            time.sleep(gap)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--w", type=int, default=960)
    ap.add_argument("--ms", type=int, default=250)
    ap.add_argument("--n", type=int, default=1)
    ap.add_argument("--gap", type=float, default=0.4)
    a = ap.parse_args()
    if a.cmd == "shot":
        shot(a.args[0] if a.args else "s", a.w)
    elif a.cmd == "key":
        for i in range(a.n):
            key(a.args[0], a.ms)
            if i + 1 < a.n:
                time.sleep(a.gap)
    elif a.cmd == "touch":
        for i in range(a.n):
            touch(a.args[0], a.ms)
            if i + 1 < a.n:
                time.sleep(a.gap)
    elif a.cmd == "tap":
        tap(float(a.args[0]), float(a.args[1]))
    elif a.cmd in ("stick", "rstick"):
        stick(float(a.args[0]), float(a.args[1]), a.ms, a.cmd == "rstick")
    elif a.cmd == "seq":
        seq(a.args[0], a.gap)
    else:
        sys.exit("unknown command " + a.cmd)


if __name__ == "__main__":
    main()
