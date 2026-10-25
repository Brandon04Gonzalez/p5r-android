# Replays a recorded touch scenario on the tablet (private/p5r/rec/<name>: ev.txt from `getevent -t`, log.txt from
# `logcat -v monotonic` with the game's trace_tasks on, both on CLOCK_MONOTONIC).
#
# Each gesture (first finger down to last finger up) is tied to the last anchor before it in the recording: a change
# of the engine's task list (`p5r: tasks ... +A -B`), a scene start/end, or the Android activity being displayed. The
# replay follows the live log, matches the same anchors in order (skipping ones that do not come), and injects a
# gesture once its anchor has come and the same delay has passed. Loads and frame rates that differ from the
# recording then only move the gestures, they do not desync them.
#
# Usage: python replay.py <rec dir> [--from-launch] [--until SECONDS] [--dry]
#   --from-launch  force-stops the game, starts its launcher and replays from there (the recorded taps on the home
#                  screen before the launcher are skipped)
#   --until S      stops after the gestures of the first S seconds of the recording
import argparse
import os
import re
import subprocess
import sys
import threading
import time

ADB = os.path.expandvars(r"%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe")
SERIAL = os.environ.get("ANDROID_SERIAL", "")  # adb -s; empty: the only device connected
DEV = ["-s", SERIAL] if SERIAL else []
PKG = "io.github.p5rrecomp"
TOUCH = "/dev/input/event2"
HERE = os.path.dirname(os.path.abspath(__file__))
INJECT_LOCAL = os.path.join(HERE, "..", "..", "..", "..", "private", "p5r", "injectbuild", "classes.dex")
INJECT = "/data/local/tmp/inject.dex"

EV_RE = re.compile(r"\[\s*([\d.]+)\]\s+(/dev/input/event\d+):\s+([0-9a-f]{4})\s+([0-9a-f]{4})\s+([0-9a-f]{8})")
LOG_RE = re.compile(r"^\s*([\d.]+)\s+\d+\s+\d+\s+\w\s+(\S+)\s*:\s(.*)$")


def anchors_of(tag, text):
    """The anchor signatures of a log line: one per task added or removed (the engine groups the changes of a frame
    in one line, and a replay groups them differently), a scene start or end, the Android activity displayed."""
    if tag == "p5r":
        m = re.match(r"p5r: tasks \S+ \(\d+\) (.*)", text)
        if m:
            return m.group(1).split()
        if text.startswith("p5r: scene "):
            return [text.split(" (")[0]]
        return []
    if tag == "ActivityTaskManager" and text.startswith("Displayed " + PKG):
        return ["displayed " + text.split(" for ")[0].split()[1]]
    return []


def to_display(x, y):
    """Panel units (portrait, 12000 x 19200) to display pixels with the display at ROTATION_90 (1920 x 1200, the
    game's and the launcher's landscape): checked against the launcher's Jugar button."""
    return round(y / 10.0, 1), round(1200 - x / 10.0, 1)


def load(rec):
    """Gestures as lists of frames (t, {finger id: (x, y)}), from first finger down to last finger up, and the
    anchors (t, signature)."""
    # slots: slot -> tracking id of the finger down there; pos: slot -> [x, y], kept after a release (the kernel does
    # not repeat a coordinate that did not change, even for the next finger in that slot)
    frames, slots, pos, slot, pending = [], {}, {}, 0, False
    with open(os.path.join(rec, "ev.txt"), errors="replace") as f:
        for line in f:
            m = EV_RE.match(line)
            if not m or m.group(2) != TOUCH:
                continue
            t, ty, co, v = float(m.group(1)), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)
            if v >= 1 << 31:
                v -= 1 << 32
            if ty == 3 and co == 0x2F:  # ABS_MT_SLOT
                slot = v
            elif ty == 3 and co == 0x39:  # ABS_MT_TRACKING_ID
                if v < 0:
                    slots.pop(slot, None)
                else:
                    slots[slot] = v
                pending = True
            elif ty == 3 and co in (0x35, 0x36):  # ABS_MT_POSITION_X / Y
                pos.setdefault(slot, [0, 0])[0 if co == 0x35 else 1] = v
                pending = True
            elif ty == 0 and co == 0 and pending:  # SYN_REPORT: one frame
                # The slot is the pointer id (Android takes 0..31; the kernel's tracking ids are larger).
                frames.append((t, {sl: to_display(*pos.get(sl, [0, 0])) for sl in slots}))
                pending = False
    gestures, cur = [], []
    for fr in frames:
        if fr[1] or cur:
            cur.append(fr)
        if cur and not fr[1]:
            gestures.append(cur)
            cur = []
    anchors = []  # (t, signature)
    with open(os.path.join(rec, "log.txt"), errors="replace") as f:
        for line in f:
            m = LOG_RE.match(line)
            if m:
                for a in anchors_of(m.group(2), m.group(3)):
                    anchors.append((float(m.group(1)), a))
    return gestures, anchors


def adb(*args, **kw):
    return subprocess.run([ADB, *DEV, *args], capture_output=True, text=True, **kw)


class LogFollower(threading.Thread):
    """Follows the device log and matches the recorded anchors: each live anchor takes the first unmatched recorded
    one with the same signature among the next `window` after the oldest unmatched one, so anchors that come in
    another order still match their own."""

    def __init__(self, anchors, window=60):
        super().__init__(daemon=True)
        self.anchors, self.window = anchors, window
        self.low = 0  # the oldest anchor not matched or given up
        self.high = -1  # the newest anchor matched
        self.matched = {}  # recorded anchor index -> host time it came
        self.given_up = set()
        self.lock = threading.Lock()
        env = dict(os.environ, MSYS_NO_PATHCONV="1")
        self.proc = subprocess.Popen([ADB, *DEV, "logcat", "-v", "monotonic", "-T", "1", "p5r:V",
                                      "ActivityTaskManager:I", "*:S"], stdout=subprocess.PIPE, text=True,
                                     errors="replace", env=env)

    def run(self):
        for line in self.proc.stdout:
            m = LOG_RE.match(line)
            if not m:
                continue
            now = time.monotonic()
            for a in anchors_of(m.group(2), m.group(3)):
                with self.lock:
                    # From the oldest open anchor to a window past the newest matched one; anchors that fell a
                    # window behind the newest matched one did not come in this run.
                    for j in range(self.low, min(max(self.low, self.high + 1) + self.window, len(self.anchors))):
                        if j not in self.matched and j not in self.given_up and self.anchors[j][1] == a:
                            self.matched[j] = now
                            self.high = max(self.high, j)
                            break
                    self.advance()

    def advance(self):
        while self.low < len(self.anchors) and (self.low in self.matched or self.low in self.given_up or
                                               self.low < self.high - self.window):
            self.low += 1

    def when(self, index):
        with self.lock:
            return self.matched.get(index)

    def later_since(self, index):
        """When the first anchor after `index` came (None if none has)."""
        with self.lock:
            later = [t for j, t in self.matched.items() if j > index]
            return min(later) if later else None

    def give_up(self, index, at):
        with self.lock:
            self.matched.setdefault(index, at)
            self.high = max(self.high, index)
            self.advance()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rec")
    ap.add_argument("--from-launch", action="store_true")
    ap.add_argument("--until", type=float, default=0)
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--rescue-a", type=float, default=0, help="tap A every S seconds while an anchor is late")
    ap.add_argument("--rare", type=int, default=2, help="anchors seen at most this many times in the recording")
    ap.add_argument("--hook", action="append", default=[],
                    help="T:command, an adb shell command run once the replay passes second T of the recording")
    ap.add_argument("--max-wait", type=float, default=120, help="seconds to wait for an anchor before going on")
    ap.add_argument("--skip-wait", type=float, default=6,
                    help="seconds to keep waiting for an anchor once a later one has come")
    args = ap.parse_args()
    gestures, anchors = load(args.rec)
    t0 = gestures[0][0][0] if gestures else 0
    print(f"{len(gestures)} gestures, {len(anchors)} anchors, {gestures[-1][-1][0] - t0:.0f} s recorded", flush=True)
    # Each gesture: the last anchor at or before its first event, and the delay after it. Only rare anchors (twice
    # or less in the recording): tasks that come and go with where the player walks (fld_epl, fld_camera_interp,
    # bustup...) come a different number of times in a replay and would tie a gesture to the wrong moment.
    count = {}
    for _, sig in anchors:
        count[sig] = count.get(sig, 0) + 1
    usable = [i for i, (_, sig) in enumerate(anchors) if count[sig] <= args.rare]
    plan = []
    for g in gestures:
        start = g[0][0]
        if args.until and start - t0 > args.until:
            break
        a = max((i for i in usable if anchors[i][0] <= start), default=-1)
        plan.append((g, a, start - (anchors[a][0] if a >= 0 else t0)))
    if args.from_launch:
        # Skip what came before the launcher was displayed (the home screen's icon tap).
        first = next((i for i, (_, s) in enumerate(anchors) if s.startswith("displayed ")), None)
        if first is not None:
            plan = [p for p in plan if p[1] >= first]
    if args.dry:
        for g, a, d in plan[:40]:
            print(f"{g[0][0] - t0:8.2f}s  anchor {a:4d} {anchors[a][1][:60] if a >= 0 else '-':60s} +{d:6.2f}s  "
                  f"{len(g)} frames, first {list(g[0][1].values())[:2]}")
        return
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    subprocess.run([ADB, *DEV, "push", INJECT_LOCAL, INJECT], capture_output=True, env=env)
    if args.from_launch:
        adb("shell", f"am force-stop {PKG}", env=env)
        adb("logcat", "-c", env=env)
        time.sleep(1)
    follower = LogFollower(anchors)
    follower.start()
    inj = subprocess.Popen([ADB, *DEV, "shell", f"CLASSPATH={INJECT} app_process / Inject"],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
    print("injector:", inj.stdout.readline().strip())
    if args.from_launch:
        adb("shell", f"monkey -p {PKG} -c android.intent.category.LAUNCHER 1", env=env)
    hooks = [(float(h.split(":", 1)[0]), h.split(":", 1)[1]) for h in args.hook]
    began = time.monotonic()
    prev_end_rec = prev_end_host = None  # the last gesture's end, in the recording and here
    for n, (g, a, d) in enumerate(plan):
        if a >= 0:
            waited = last_rescue = time.monotonic()
            while follower.when(a) is None:
                later = follower.later_since(a)
                now = time.monotonic()
                # A later anchor came: this one may still come (another order), or not at all in this run.
                if later is not None and now - later > args.skip_wait:
                    print(f"gesture {n}: anchor {a} ({anchors[a][1][:50]}) did not come, a later one did", flush=True)
                    follower.give_up(a, later)
                    break
                # A dialog line that took one more A press than in the recording: the game waits for a press and
                # the replay for the anchor after it. --rescue-a S taps A (the default layout's) every S seconds.
                if args.rescue_a and now - max(waited, last_rescue) > args.rescue_a:
                    last_rescue = now
                    print(f"gesture {n}: waiting {now - waited:.0f} s for anchor {a}, tapping A", flush=True)
                    inj.stdin.write("B\nF 0 9 1805 790\nF 80000\nS\n")
                    inj.stdin.flush()
                    inj.stdout.readline()
                if now - waited > args.max_wait:
                    print(f"gesture {n}: anchor {a} ({anchors[a][1][:50]}) never came, going on", flush=True)
                    follower.give_up(a, now)
                    break
                time.sleep(0.01)
            due = follower.when(a) + d
        else:
            due = began + d
        # Never sooner after the last gesture than in the recording: when an anchor came late, the gestures after it
        # keep their gaps instead of going out together (a run compressed that way left Joker in the subway).
        if prev_end_rec is not None:
            due = max(due, prev_end_host + (g[0][0] - prev_end_rec))
        late = time.monotonic() - due
        if late < 0:
            time.sleep(-late)
        for h in list(hooks):
            if g[0][0] - t0 >= h[0]:
                hooks.remove(h)
                print(f"hook at rec {h[0]:.0f}s: {h[1]}", flush=True)
                adb("shell", h[1], env=env)
        lines = ["B"]
        prev = g[0][0]
        for t, fingers in g:
            pts = " ".join(f"{i} {x} {y}" for i, (x, y) in fingers.items())
            lines.append(f"F {int(round((t - prev) * 1e6))} {pts}".rstrip())
            prev = t
        lines.append("S")
        inj.stdin.write("\n".join(lines) + "\n")
        inj.stdin.flush()
        inj.stdout.readline()
        prev_end_rec, prev_end_host = g[-1][0], time.monotonic()
        if n % 10 == 0 or late > 1:
            print(f"gesture {n}/{len(plan)} at rec {g[0][0] - t0:7.1f}s, anchor {a} ({anchors[a][1][:40] if a >= 0 else '-'})"
                  f"{f', {late:.1f} s late' if late > 0.05 else ''}", flush=True)
    print("done in", round(time.monotonic() - began), "s")
    inj.stdin.close()


if __name__ == "__main__":
    main()
