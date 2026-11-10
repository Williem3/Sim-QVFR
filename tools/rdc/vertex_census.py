"""Vertex work per pass of one captured frame: what multi-view rendering by instancing draws twice.

Runs inside RenderDoc's UI process (D3D11):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<report.md>'
    $env:RDC_RANGES = 'eye0=12841-27211;eye1=27216-42692;focus0=43349-52804;focus1=53461-62902'   (event ranges)
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\vertex_census.py
For each range: its draws, and those multi-view doubles (games/lmu/mvr_pass.cpp: from the first draw into a multisample
colour target with a depth target, every draw with a depth target bound; the post-processing has none), with the
pipeline statistics RenderDoc counts per draw: vertices read by the input assembler, vertex shader invocations,
primitives rasterized, pixel shader invocations. Instancing runs the vertex shader and its vertex fetch once more for
every doubled draw (NVIDIA's single-pass stereo doesn't): the doubled draws' VS invocations are that extra work.
The targets the doubled draws go to are listed with their share. Counters only (one replay of the frame with queries);
RenderDoc's per-draw durations are left out (mostly its own overhead).
"""
import collections
import os
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
RANGES = os.environ.get("RDC_RANGES", "")
out = open(OUT, "w", encoding="utf-8")


def p(*a):
    out.write(" ".join(str(x) for x in a) + "\n")
    out.flush()


def run():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(IN, "", None) != rd.ResultCode.Succeeded:
        p("open failed")
        return
    st, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if st != rd.ResultCode.Succeeded:
        p("replay failed", st)
        return
    tex = {int(t.resourceId): t for t in ctl.GetTextures()}

    def tdesc(rid):
        t = tex.get(int(rid))
        if not t:
            return "?"
        fmt = t.format.Name() if hasattr(t.format, "Name") else str(t.format)
        return f"{t.width}x{t.height} {fmt}" + (f" x{t.msSamp}" if t.msSamp > 1 else "")

    draws = []

    def walk(a):
        if a.flags & rd.ActionFlags.Drawcall:
            draws.append(a)
        for c in a.children:
            walk(c)

    for a in ctl.GetRootActions():
        walk(a)
    p(f"# Vertex census: {os.path.basename(IN)}\n")
    p(f"{len(draws)} draws in the frame\n")

    wanted = [rd.GPUCounter.InputVerticesRead, rd.GPUCounter.VSInvocations, rd.GPUCounter.RasterizedPrimitives,
              rd.GPUCounter.PSInvocations]
    have = set(ctl.EnumerateCounters())
    counters = [c for c in wanted if c in have]
    p("counters:", ", ".join(ctl.DescribeCounter(c).name for c in counters))
    values = collections.defaultdict(dict)
    for r in ctl.FetchCounters(counters):
        values[r.eventId][r.counter] = r.value.u64
    names = {rd.GPUCounter.InputVerticesRead: "ia", rd.GPUCounter.VSInvocations: "vs",
             rd.GPUCounter.RasterizedPrimitives: "prims", rd.GPUCounter.PSInvocations: "ps"}

    ranges = []
    for part in [x for x in RANGES.split(";") if x.strip()]:
        name, span = part.split("=")
        lo, hi = span.split("-")
        ranges.append((name.strip(), int(lo), int(hi)))
    if not ranges:
        ranges = [("frame", 0, 1 << 30)]

    def sums(ds):
        s = collections.Counter()
        for d in ds:
            for c, v in values.get(d.eventId, {}).items():
                s[names[c]] += v
            s["indices"] += d.numIndices * max(1, d.numInstances)
        return s

    def ms(rid):
        t = tex.get(int(rid))
        return t is not None and t.msSamp > 1

    p("\n| range | draws | doubled | VS invocations (all / doubled) | IA vertices (doubled) | rasterized prims (doubled) | PS invocations (doubled) |")
    p("|---|---|---|---|---|---|---|")
    details = []
    for name, lo, hi in ranges:
        ds = [d for d in draws if lo <= d.eventId <= hi]
        start = next((d.eventId for d in ds if d.depthOut != rd.ResourceId.Null() and any(
            ms(o) for o in d.outputs if o != rd.ResourceId.Null())), None)
        doubled = [d for d in ds if start is not None and d.eventId >= start and d.depthOut != rd.ResourceId.Null()]
        a, b = sums(ds), sums(doubled)
        p(f"| {name} | {len(ds)} | {len(doubled)} | {a['vs']:,} / {b['vs']:,} | {b['ia']:,} | {b['prims']:,} | {b['ps']:,} |")
        by = collections.Counter()
        byDraws = collections.Counter()
        for d in doubled:
            o = next((x for x in d.outputs if x != rd.ResourceId.Null()), rd.ResourceId.Null())
            key = (tdesc(o) if o != rd.ResourceId.Null() else "no colour") + " / depth " + tdesc(d.depthOut)
            by[key] += values.get(d.eventId, {}).get(rd.GPUCounter.VSInvocations, 0)
            byDraws[key] += 1
        biggest = sorted(doubled, key=lambda d: -values.get(d.eventId, {}).get(rd.GPUCounter.VSInvocations, 0))[:5]
        details.append((name, start, by, byDraws, biggest, sums([d for d in ds if start is None or d.eventId < start])))
    for name, start, by, byDraws, biggest, before in details:
        p(f"\n## {name}\n")
        p(f"multi-view's window starts at event {start}; before it (shadows, probes): VS invocations {before['vs']:,}, "
          f"IA vertices {before['ia']:,}\n")
        for k, v in by.most_common(12):
            p(f"- {k}: {byDraws[k]} draws, {v:,} VS invocations")
        p("\nlargest doubled draws:")
        for d in biggest:
            v = values.get(d.eventId, {})
            p(f"- event {d.eventId}: {d.numIndices} indices x{max(1, d.numInstances)}, VS {v.get(rd.GPUCounter.VSInvocations, 0):,}"
              f", PS {v.get(rd.GPUCounter.PSInvocations, 0):,}")
    ctl.Shutdown()
    cap.Shutdown()
    p("\ndone")


try:
    run()
except Exception:
    p(traceback.format_exc())
out.close()
os._exit(0)
