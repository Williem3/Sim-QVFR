"""A test run's session log as a report: what ran, what changed when, how it performed, what went wrong.

Usage: python session_report.py <session-*.log> [more logs ...] [--all-problems]
Works for any game's log (the layer's own lines; game-specific lines are only counted). Markdown on stdout, for the
recon notes' "Test runs" section. Settings changed live in the headset menu are listed with their times: a setting that
was on for ten seconds between two periodic summaries still shows (the summaries alone miss it).
"""
import argparse
import collections
import re
import statistics
import sys
from datetime import datetime, timedelta

LINE = re.compile(r"^(\d\d:\d\d:\d\d\.\d{3}) \[(\d+)\] (.*)$")
PROBLEM = re.compile(r"(?i)\b(error|failed|fail|exception|crash|hang|refused|not written|lost|invalid|unsupported|"
                     r"can't|cannot|missing|timeout|timed out|denied|anti-cheat)\b")
NOT_PROBLEM = re.compile(r"(?i)result=0\b|0 errors|no errors|error\(s\) 0|^hang watch: (\d+ s;|dump helper standing by)|"
                         r"^probe: not .*; probes refused")


def ts(s):
    return datetime.strptime(s, "%H:%M:%S.%f")


def fmt_dur(d):
    s = int(d.total_seconds())
    return f"{s // 60}m{s % 60:02d}s"


def report(path, all_problems):
    lines = []
    with open(path, "rb") as f:
        for rawb in f:
            try:  # the layer writes UTF-8, but some lines carry the ANSI code page (the degree sign)
                raw = rawb.decode("utf-8")
            except UnicodeDecodeError:
                raw = rawb.decode("cp1252", errors="replace")
            m = LINE.match(raw.rstrip("\r\n"))
            if m:
                lines.append((ts(m.group(1)), m.group(1), int(m.group(2)), m.group(3)))
    if not lines:
        print(f"# {path}\n\nNo log lines.\n")
        return
    # Times wrap at midnight: keep them increasing.
    fixed, day = [], timedelta(0)
    prev = lines[0][0]
    for t, s, tid, msg in lines:
        if t + day < prev - timedelta(hours=1):
            day += timedelta(days=1)
        prev = t + day
        fixed.append((t + day, s, tid, msg))
    lines = fixed
    t0, t1 = lines[0][0], lines[-1][0]
    msgs = [m for _, _, _, m in lines]

    def first(pattern):
        r = re.compile(pattern)
        for t, s, _, m in lines:
            if r.search(m):
                return s, m
        return None, None

    def all_of(pattern):
        r = re.compile(pattern)
        return [(t, s, m) for t, s, _, m in lines if r.search(m)]

    print(f"# Session report: {path.replace(chr(92), '/').split('/')[-1]}\n")
    print(f"- {lines[0][1]} to {lines[-1][1]} ({fmt_dur(t1 - t0)}), {len(lines):,} lines, "
          f"{len({tid for _, _, tid, _ in lines})} threads")

    print("\n## Launch\n")
    for pat in (r"^xrCreateApiLayerInstance app=", r"^experiment: mode=", r"^experiment: .*periphery",
                r"^experiment: FOV crop", r"^experiment: focus shape", r"has no QVFR yet", r"anti-cheat is running",
                r"^layers below this one", r"^gaze: XR_EXT_eye_gaze", r"^cheeky: runtime",
                r"^views4 dlss: (each|off|NGX)",
                r"^xrEnumerateViewConfigurationViews .*view=0", r"^xrCreateSession result", r"^image dump:"):
        for t, s, m in all_of(pat)[-1:]:
            print(f"- `{m[:220]}`")
    exts = sorted({m.split()[1] for m in msgs if m.startswith("  extension ")})
    if exts:
        print(f"- Game's extensions: {', '.join(exts)}")

    print("\n## Session\n")
    states = all_of(r"^session state: ")
    if states:
        print("- States: " + ", ".join(f"{m.split(': ', 1)[1]} {s[:8]}" for _, s, m in states))
    swaps = collections.Counter(re.sub(r"^xrCreateSwapchain ", "", m) for _, _, m in all_of(r"^xrCreateSwapchain "))
    for sw, n in swaps.items():
        print(f"- Swapchain {sw[:160]}" + (f" (x{n})" if n > 1 else ""))
    frames = [int(m.group(1)) for m in (re.search(r"^xrEndFrame frame=(\d+)", x) for x in msgs) if m]
    if frames:
        first_frame = first(r"^xrEndFrame frame=1 ")[0]
        print(f"- Frames: up to {max(frames):,} (first at {first_frame})")
    ended = [m for m in msgs[-40:] if re.search(r"xrDestroySession|EXITING|xrDestroyInstance", m)]
    no_teardown = ("no session teardown logged: some games quit without destroying the session (LMU); a crash or "
                   "kill looks the same here, so check the game's own log")
    print(f"- Ending: {'clean (' + ended[-1][:80] + ')' if ended else no_teardown}")
    print(f"- Last line: `{lines[-1][1]} {lines[-1][3][:200]}`")

    print("\n## Settings changed in the run\n")
    changes = all_of(r"^settings menu: .*->|^live ini|^live: reloaded|^gaze: first tracked|^gaze: .*(on|off)$")
    if changes:
        for _, s, m in changes:
            print(f"- {s[:8]} `{m[:200]}`")
    else:
        print("- None logged")
    # The periodic `live:` lines come in kinds (focus box, shape, periphery): list a kind again only when it changes.
    last, distinct = {}, []
    for _, s, m in all_of(r"^live: "):
        kind = " ".join(m.split()[1:3])
        if last.get(kind) != m:
            distinct.append((s, m, kind in last))
            last[kind] = m
    if any(changed for _, _, changed in distinct):
        print("\nLive state at the start, then each change:")
        for s, m, changed in distinct[:60]:
            print(f"- {s[:8]} {'changed: ' if changed else ''}`{m[:200]}`")

    print("\n## Performance\n")
    fps, p90, layer_ms, long_frames = [], [], [], []
    for _, s, m in all_of(r"pacing: display period"):
        a = re.search(r"\(([\d.]+) fps over", m)
        b = re.search(r"p90 ([\d.]+)", m)
        c = re.search(r"display periods (\d+)/(\d+)/(\d+)/(\d+)", m)
        if a:
            fps.append(float(a.group(1)))
        if b:
            p90.append(float(b.group(1)))
        if c:
            long_frames.append(sum(int(c.group(i)) for i in (2, 3, 4)))
    for _, s, m in all_of(r"cpu: ms per call .*xrEndFrame"):
        a = re.search(r"xrEndFrame [\d.]+ \(layer ([\d.]+)", m)
        if a:
            layer_ms.append(float(a.group(1)))
    if fps:
        steady = fps[1:] or fps
        print(f"- fps per summary: min {min(steady):.1f}, median {statistics.median(steady):.1f}, max {max(steady):.1f} "
              f"({len(fps)} summaries; the first, loading, left out)")
    if p90:
        print(f"- Frame interval p90: median {statistics.median(p90):.2f} ms, worst {max(p90):.2f} ms")
    if long_frames:
        print(f"- Frames over one display period: {sum(long_frames[1:] or long_frames):,} in total")
    if layer_ms:
        print(f"- Layer's own xrEndFrame time: median {statistics.median(layer_ms):.3f} ms, worst {max(layer_ms):.3f} ms")
    if not (fps or layer_ms):
        print("- No pacing summaries (identity runs and short runs have none)")

    print("\n## Captures and dumps\n")
    caps = all_of(r"(?i)capture: (wrote|saved|armed)|image dump: wrote|\.rdc|minidump|hang-.*\.dmp")
    for _, s, m in caps[:30]:
        print(f"- {s[:8]} `{m[:200]}`")
    if not caps:
        print("- None")

    print("\n## Problems\n")
    probs = collections.OrderedDict()
    for _, s, m in all_of(PROBLEM.pattern):
        # A count of zero ("failed 0", "lost 0") is a good result, not a problem.
        real = [x for x in PROBLEM.finditer(m) if not re.match(r"\s*[:=]?\s*0\b", m[x.end():])]
        if NOT_PROBLEM.search(m) or not real:
            continue
        key = re.sub(r"\d+(\.\d+)?", "N", m)[:160]
        if key not in probs:
            probs[key] = [s, m, 0]
        probs[key][2] += 1
    # ACE views4 with Cheeky: a periphery view whose DLSS output Cheeky takes at another size than its rect in the
    # output layout (ACE leaves the last-made feature's OutWidth in NGX's parameters; recon-ace [dlss-extent]).
    layout = all_of(r"^views4: viewport layout output=")
    rects = re.findall(r"(\d): \d+, (\d+x\d+);", layout[-1][2]) if layout else []
    periphery = {size for view, size in rects if view in "01"}
    if periphery:
        for _, s, m in all_of(r"^cheeky: view .* out (\d+x\d+), eye \w+, region Periphery")[-2:]:
            out = re.search(r" out (\d+x\d+),", m).group(1)
            if out not in periphery:
                eye = re.search(r"eye (\w+)", m).group(1)
                note = (f"cheeky: {eye} periphery view composed at {out}, its rect is {'/'.join(sorted(periphery))}: "
                        f"the periphery won't match the focus box (stale NGX OutWidth; views4_dlss_extent)")
                probs.setdefault(f"cheeky periphery {eye} {out}", [s, note, 0])[2] += 1
    if probs:
        items = list(probs.values())
        for s, m, n in items if all_problems else items[:40]:
            print(f"- {s[:8]} `{m[:220]}`" + (f" (x{n})" if n > 1 else ""))
        if not all_problems and len(items) > 40:
            print(f"- ... {len(items) - 40} more kinds (--all-problems)")
    else:
        print("- None")

    print("\n## Game-specific lines (by prefix)\n")
    prefixes = collections.Counter(m.split(":", 1)[0] for m in msgs if ":" in m[:40])
    common = {"experiment", "session state", "settings menu", "live", "gaze", "input", "xrLocateViews", "compositor"}
    for p, n in prefixes.most_common(25):
        if p not in common:
            print(f"- `{p}`: {n}")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--all-problems", action="store_true")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    for p in args.logs:
        report(p, args.all_problems)


if __name__ == "__main__":
    main()
