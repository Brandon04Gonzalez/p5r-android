# Copies the public part of this repository into <out> for the port's own GitHub repository:
#   - the files git tracks (as they are in the working tree), minus what is not this project's to publish (the
#     upstream project's game screenshots, Wonder's launcher art and fonts, the other games' glue) or only local
#     (the work log, with this PC's paths and devices),
#   - games/p5r/public at the top (README.md, .github/ issue templates),
#   - a launcher icon with no game art (make_text_icon.py) instead of games/p5r/icon.
# Never reads private/. Ends with a scan for personal data (paths, e-mail, device serials) in what it wrote.
#   python games/p5r/tools/export_public.py <out dir>
# <out dir> must be outside this repository; it may be a clone of the public repository: everything but .git and
# private/ is replaced.
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
EXCLUDE = [
    r"^private/",
    r"^docs/media/",
    r"^docs/WONDER-",
    r"^(README|PROGRESS|CONTRIBUTING)\.md$",
    r"^android/wonder/(?!importer/)",
    r"^android/launcher-mockup/",
    r"^games/(hades|wonder)/",
    r"^games/p5r/(PROGRESO\.md|icon/|public/)",
    r"^tools/[^/]*wonder[^/]*$",
]
# What must not reach the public repository (checked in every text file written): any user folder path, personal
# e-mail addresses, and the lines of private/personal_patterns.txt (regexes kept out of the repository: device
# serials, names).
PERSONAL = [r"[A-Za-z]:[\\/]+Users[\\/]+\w", r"/[a-z]/Users/\w", r"/home/[a-z]\w*/",
            r"[\w.+-]+@(gmail|hotmail|outlook|yahoo|icloud|proton)\.(com|me)"]
_local = os.path.join(ROOT, "private", "personal_patterns.txt")
if os.path.isfile(_local):
    PERSONAL += [l.strip() for l in open(_local, encoding="utf-8") if l.strip() and not l.startswith("#")]


def tracked():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    return [f for f in out.decode("utf-8").split("\0") if f]


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: export_public.py <out dir>")
    out = os.path.abspath(sys.argv[1])
    if out == ROOT or out.startswith(ROOT + os.sep):
        sys.exit("the output must be outside this repository")
    os.makedirs(out, exist_ok=True)
    for name in os.listdir(out):
        if name in (".git", "private"):  # private/: that clone's own build and links to tools (git ignores it)
            continue
        p = os.path.join(out, name)
        shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)

    copied = 0
    for f in tracked():
        if any(re.search(x, f) for x in EXCLUDE) or not os.path.isfile(os.path.join(ROOT, f)):
            continue
        dst = os.path.join(out, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, f), dst)
        copied += 1
    public = os.path.join(ROOT, "games", "p5r", "public")
    for base, _, names in os.walk(public):
        for n in names:
            src = os.path.join(base, n)
            dst = os.path.join(out, os.path.relpath(src, public))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            copied += 1
    subprocess.run([sys.executable, os.path.join(ROOT, "games", "p5r", "tools", "make_text_icon.py"),
                    os.path.join(out, "games", "p5r", "icon")], check=True)
    print(f"{copied} files copied to {out}")

    found = 0
    for base, dirs, names in os.walk(out):
        dirs[:] = [d for d in dirs if d not in (".git", "private")]
        for n in names:
            p = os.path.join(base, n)
            try:
                text = open(p, encoding="utf-8").read()
            except (UnicodeDecodeError, OSError):
                continue
            for x in PERSONAL:
                for m in re.finditer(x, text, re.I):
                    line = text.count("\n", 0, m.start()) + 1
                    print(f"PERSONAL DATA: {os.path.relpath(p, out)}:{line}: {m.group(0)}")
                    found += 1
    if found:
        sys.exit(f"{found} personal data matches: fix them before publishing")
    print("no personal data found")


if __name__ == "__main__":
    main()
