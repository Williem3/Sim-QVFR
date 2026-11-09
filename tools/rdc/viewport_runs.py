"""A frame's draws as runs of the same viewport and targets: where an engine that renders several views into one
target (ACC's instanced pairs, eye pair then focus pair inside each pass) switches from one view to the next, which
pass_outline.py (one line per target set) merges.

Runs inside RenderDoc's UI process (any API):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<report.md>'; $env:RDC_RANGE = '6700-23848'
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\viewport_runs.py
RDC_RANGE: the event range to walk (default: the whole frame). Each run: its events, draws (instanced draws and the
largest instance count), the viewport, the first colour target (or the depth target) and its size.
"""
import os
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
RANGE = os.environ.get("RDC_RANGE", "")
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
    lo, hi = (int(x) for x in RANGE.split("-")) if RANGE else (0, 1 << 31)
    tex = {int(t.resourceId): t for t in ctl.GetTextures()}
    names = {int(r.resourceId): r.name for r in ctl.GetResources()}

    def tname(rid):
        t = tex.get(int(rid))
        if not t:
            return "-"
        return f"{names.get(int(rid), int(rid))} {t.width}x{t.height}"

    draws = []

    def walk(actions):
        for a in actions:
            if a.children:
                walk(a.children)
            if (a.flags & rd.ActionFlags.Drawcall) and lo <= a.eventId <= hi:
                draws.append(a)

    walk(ctl.GetRootActions())
    p(f"# Viewport runs: {os.path.basename(IN)} events {lo}-{hi if hi < (1 << 31) else 'end'}, {len(draws)} draws")
    cur = None
    for a in draws:
        ctl.SetFrameEvent(a.eventId, False)
        v = ctl.GetPipelineState().GetViewport(0)
        outs = [o for o in a.outputs if int(o)]
        target = tname(outs[0]) if outs else "depth " + tname(a.depthOut)
        key = (round(v.x), round(v.y), round(v.width), round(abs(v.height)), target)
        inst = max(1, a.numInstances)
        if cur and cur["key"] == key:
            cur["last"] = a.eventId
            cur["draws"] += 1
            cur["inst"] += inst > 1
            cur["maxInst"] = max(cur["maxInst"], inst)
            continue
        if cur:
            emit(cur)
        cur = {"key": key, "first": a.eventId, "last": a.eventId, "draws": 1, "inst": int(inst > 1), "maxInst": inst}
    if cur:
        emit(cur)
    ctl.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()


def emit(r):
    x, y, w, h, target = r["key"]
    p(f"- **{r['first']}-{r['last']}** draws {r['draws']} (instanced {r['inst']}, up to {r['maxInst']}): viewport "
      f"{x},{y} {w}x{h}; {target}")


try:
    run()
except Exception:
    p(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
