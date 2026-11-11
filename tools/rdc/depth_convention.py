"""How a pass uses its depth buffer: the depth test, the clear value, and the projection its draws really use.

Runs inside RenderDoc's UI process (D3D11):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<report.md>'; $env:RDC_EYE = '<first>-<last>'
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\depth_convention.py
For a few draws spread over the range whose vertex shader declares viewMatrix and viewProjMatrix: the depth function
and write mask, the view matrix, and the projection as inverse(view) x viewProj (row vectors), with its third and
fourth rows: 0 at the near plane and 1 far away, or the other way round. Then every depth clear in the range.
"""
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
FIRST, LAST = (int(x) for x in os.environ["RDC_EYE"].split("-"))
out = open(OUT, "w", encoding="utf-8")


def p(*a):
    out.write(" ".join(str(x) for x in a) + "\n")
    out.flush()


def rid_of(u):
    d = getattr(u, "descriptor", u)
    rid = getattr(d, "resource", None)
    if rid is None:
        rid = getattr(d, "resourceId", None)
    return rid, int(getattr(d, "byteOffset", 0)), int(getattr(d, "byteSize", 0))


def mul(a, b):
    return [sum(a[i * 4 + k] * b[k * 4 + j] for k in range(4)) for i in range(4) for j in range(4)]


def invert(m):
    a = [[m[i * 4 + j] for j in range(4)] + [1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
    for c in range(4):
        piv = max(range(c, 4), key=lambda r: abs(a[r][c]))
        if abs(a[piv][c]) < 1e-30:
            return None
        a[c], a[piv] = a[piv], a[c]
        d = a[c][c]
        a[c] = [x / d for x in a[c]]
        for r in range(4):
            if r != c and a[r][c] != 0:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [a[i][j + 4] for i in range(4) for j in range(4)]


def rows(m):
    return " | ".join(" ".join(f"{m[i * 4 + j]:.6g}" for j in range(4)) for i in range(4))


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
    sf = ctl.GetStructuredFile()
    draws, events = [], []

    def walk(a):
        for e in a.events:
            if FIRST <= e.eventId <= LAST:
                events.append((e.eventId, e.chunkIndex))
        if a.flags & rd.ActionFlags.Drawcall and FIRST <= a.eventId <= LAST and int(a.depthOut):
            draws.append(a)
        for c in a.children:
            walk(c)

    for r in ctl.GetRootActions():
        walk(r)
    p(f"# Depth convention: {os.path.basename(IN)}, events {FIRST}-{LAST}, {len(draws)} draws with a depth target\n")
    picks = [draws[i] for i in sorted({len(draws) // 6, len(draws) // 3, len(draws) // 2, 2 * len(draws) // 3, len(draws) - 5})
             if 0 <= i < len(draws)]
    for a in picks:
        ctl.SetFrameEvent(a.eventId, False)
        pipe = ctl.GetPipelineState()
        d3d = ctl.GetD3D11PipelineState()
        ds = d3d.outputMerger.depthStencilState
        p(f"## Event {a.eventId}\n")
        p(f"- depth test {'on' if ds.depthEnable else 'off'}, function {ds.depthFunction}, writes {'on' if ds.depthWrites else 'off'}")
        try:
            v = pipe.GetViewport(0)
            p(f"- viewport {v.x:.0f},{v.y:.0f} {v.width:.0f}x{v.height:.0f} depth {v.minDepth:g}..{v.maxDepth:g}")
        except Exception as e:
            p(f"- viewport: {e!r}")
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        found = False
        for i, cb in enumerate(refl.constantBlocks if refl else []):
            offs = {v.name: v.byteOffset for v in cb.variables}
            if "viewMatrix" in offs and "viewProjMatrix" in offs:
                rid, off, size = rid_of(pipe.GetConstantBlock(rd.ShaderStage.Vertex, i, 0))
                data = ctl.GetBufferData(rid, off, min(size, 512))
                view = list(struct.unpack_from("<16f", data, offs["viewMatrix"]))
                vp = list(struct.unpack_from("<16f", data, offs["viewProjMatrix"]))
                p(f"- viewMatrix (as stored): {rows(view)}")
                p(f"- viewProjMatrix (as stored): {rows(vp)}")
                for label, vv, pp in (("as stored (row vectors)", view, vp),
                                      ("transposed (column vectors stored)", [view[j * 4 + i] for i in range(4) for j in range(4)],
                                       [vp[j * 4 + i] for i in range(4) for j in range(4)])):
                    inv = invert(vv)
                    if inv:
                        proj = mul(inv, pp)
                        p(f"- projection = inverse(view) x viewProj, {label}: {rows(proj)}")
                found = True
                break
        if not found:
            p("- its vertex shader declares no viewMatrix + viewProjMatrix")
        p("")
    p("## Depth clears in the range\n")
    for eid, chunk in events:
        c = sf.chunks[chunk]
        if "ClearDepthStencilView" in c.name:
            vals = {ch.name: (ch.AsFloat() if ch.type.basetype == rd.SDBasic.Float else ch.AsInt()) for ch in c.children
                    if ch.type.basetype in (rd.SDBasic.Float, rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger)}
            p(f"- event {eid}: {c.name} {vals}")
    ctl.Shutdown()
    cap.Shutdown()
    p("\ndone")


try:
    run()
except Exception:
    p(traceback.format_exc())
out.close()
os._exit(0)
