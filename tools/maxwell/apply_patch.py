#!/usr/bin/env python3
"""Minimal stand-in for `patch --quiet --forward -o <out> <in> <patchfile>` (unified diffs, one file per patch).

Used by tools/maxwell/spirv_harness/translator/Ryujinx.Graphics.Shader.csproj on Windows, which has no `patch`
(macOS keeps using the real one). Only what that csproj needs:
  - the patch may start with free text; the diff starts at the first "--- " line followed by "+++ ";
  - hunks must apply exactly (context and removed lines), at the stated line or at the nearest offset, like patch
    without fuzz; otherwise nothing is written and the exit status is 1;
  - CR/LF differences are ignored when matching (a Windows checkout may give the .patch CRLF line ends), and the
    output keeps the line ends of <in>.

Usage: python tools/maxwell/apply_patch.py [--quiet] [--forward] -o <out> <in> <patchfile>
"""
import re
import sys

HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def parse_hunks(text):
    lines = text.splitlines()
    i = 0
    while i < len(lines) and not (lines[i].startswith("--- ") and i + 1 < len(lines) and lines[i + 1].startswith("+++ ")):
        i += 1
    if i >= len(lines):
        raise ValueError("no unified diff (--- / +++) found")
    i += 2
    hunks = []
    while i < len(lines):
        m = HUNK.match(lines[i])
        if not m:
            if lines[i].startswith("--- "):
                raise ValueError("more than one file in the patch")
            i += 1
            continue
        old_start, old_len = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
        new_len = int(m.group(4) if m.group(4) is not None else 1)
        i += 1
        old, new = [], []
        while i < len(lines) and (len(old) < old_len or len(new) < new_len):
            line = lines[i]
            tag, body = (line[:1], line[1:]) if line else (" ", "")
            if tag == " ":
                old.append(body)
                new.append(body)
            elif tag == "-":
                old.append(body)
            elif tag == "+":
                new.append(body)
            elif tag != "\\":  # "\ No newline at end of file"
                raise ValueError(f"bad hunk line {i + 1}: {line!r}")
            i += 1
        if len(old) != old_len or len(new) != new_len:
            raise ValueError(f"truncated hunk at line {old_start}")
        hunks.append((old_start, old, new))
    if not hunks:
        raise ValueError("no hunks")
    return hunks


def find(src, old, want):
    """Index where `old` matches `src`, nearest to `want` first (patch's offset search, no fuzz)."""
    for delta in range(0, len(src) + 1):
        for pos in (want - delta, want + delta) if delta else (want,):
            if 0 <= pos <= len(src) - len(old) and src[pos:pos + len(old)] == old:
                return pos
    return -1


def apply(src_text, hunks):
    eol = "\r\n" if "\r\n" in src_text else "\n"
    final_eol = src_text.endswith(("\n", "\r"))
    src = [l.rstrip("\r") for l in src_text.splitlines()]
    out, cursor, offset = [], 0, 0
    for n, (start, old, new) in enumerate(hunks, 1):
        old = [l.rstrip("\r") for l in old]
        new = [l.rstrip("\r") for l in new]
        want = max(start - 1, 0) + offset
        pos = find(src, old, want)
        if pos < cursor:
            if find(src, new, want) >= cursor:
                raise ValueError(f"hunk {n}: reversed (or previously applied) patch detected")
            raise ValueError(f"hunk {n} FAILED at line {start}")
        out += src[cursor:pos] + new
        cursor = pos + len(old)
        offset = pos - (start - 1)
    out += src[cursor:]
    return eol.join(out) + (eol if final_eol else "")


def main(argv):
    args, out_path = [], None
    it = iter(argv)
    for a in it:
        if a == "-o":
            out_path = next(it, None)
        elif a in ("--quiet", "-s", "--silent", "--forward", "-N"):
            pass
        else:
            args.append(a)
    if out_path is None or len(args) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    src_path, patch_path = args
    with open(src_path, encoding="utf-8", newline="") as f:
        src_text = f.read()
    with open(patch_path, encoding="utf-8", newline="") as f:
        patch_text = f.read()
    try:
        result = apply(src_text, parse_hunks(patch_text))
    except ValueError as e:
        print(f"apply_patch: {patch_path}: {e}", file=sys.stderr)
        return 1
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
