"""Summarise a draw_inputs.py report (it runs outside RenderDoc): per draw its target, viewport, shaders, inputs,
constant buffers, and the constants whose names suggest a projection, a size or a UV transform.

Usage: python -I post_summary.py <report.md> [first event] [last event]
"""
import re
import sys

KEYS = re.compile(r"(proj|inv|view|depth|texel|size|dim|rect|scale|offset|uv|screen|viewport|resol|ratio|aspect|"
                  r"near|far|fov|radius|sigma|step|delta|exposure|lum|bloom|glare)", re.I)


def main(path, first="0", last="999999"):
    lo, hi = int(first), int(last)
    blocks, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        m = re.match(r"^## (\d+)", line)
        if m:
            cur = {"event": int(m.group(1)), "lines": []}
            blocks.append(cur)
            continue
        if cur is not None:
            cur["lines"].append(line)
    for b in blocks:
        if not lo <= b["event"] <= hi:
            continue
        out = [f"### {b['event']}"]
        stage = ""
        cb = ""
        for line in b["lines"]:
            s = line.strip()
            if s.startswith("- targets:") or s.startswith("- call:") or s.startswith("- viewport"):
                out.append("  " + s[2:])
            elif re.match(r"^- (VS|PS|CS|GS|HS|DS) ", s):
                stage = s[2:4]
                out.append("  " + s[2:])
            elif re.match(r"^- t\d+ |^- s\d+ |^- u\d+ ", s) and stage:
                out.append(f"    {stage} {s[2:]}")
            elif s.startswith("- cb "):
                cb = s[5:].rstrip(":")
                out.append(f"    {stage} cb {cb}")
            elif re.match(r"^- [A-Za-z_][\w\[\]]* = ", s) and KEYS.search(s.split("=")[0]):
                out.append(f"      {s[2:]}"[:170])
        print("\n".join(out))


if __name__ == "__main__":
    main(*sys.argv[1:])
