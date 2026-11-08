"""The outline of one captured frame: its passes in order, what each draws into, and the copies between targets.

Runs inside RenderDoc's UI process (any API; written for D3D11 and D3D12 games):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<report.md>'
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\pass_outline.py
A pass is a run of draws and dispatches with the same colour and depth targets. For each: its events, the debug marker
it sits in, the number of draws, its targets (name, size, format, MSAA, array), the first draw's viewport. Copies and
resolves are listed with source and destination. Then the targets by size, which shows a per-eye engine at a glance:
the same pass sequence twice, once per eye target (stage 3 of the QVFR playbook: the capture census).
Only the first draw of each pass is replayed (for its viewport), so it is quick even on large captures.
"""
import collections
import os
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
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
    names = {int(r.resourceId): r.name for r in ctl.GetResources()}
    tex = {int(t.resourceId): t for t in ctl.GetTextures()}
    api = ctl.GetAPIProperties().pipelineType

    def tname(rid):
        i = int(rid)
        if i == 0:
            return None
        t = tex.get(i)
        n = names.get(i, f"Resource {i}")
        if not t:
            return n
        fmt = t.format.Name() if hasattr(t.format, "Name") else str(t.format)
        extra = (f" x{t.msSamp}" if t.msSamp > 1 else "") + (f" [{t.arraysize}]" if t.arraysize > 1 else "")
        return f"{n} {t.width}x{t.height} {fmt}{extra}"

    # Flatten actions, keeping the marker path above each.
    flat = []

    def walk(a, path):
        name = a.customName or ""
        is_marker = bool(a.flags & (rd.ActionFlags.PushMarker | rd.ActionFlags.SetMarker)) or (a.children and not
                    a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch))
        sub = path + [name] if (is_marker and name) else path
        if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Copy | rd.ActionFlags.Resolve |
                      rd.ActionFlags.Clear | rd.ActionFlags.Present):
            flat.append((a, path))
        for c in a.children:
            walk(c, sub)

    for r in ctl.GetRootActions():
        walk(r, [])
    p(f"# Pass outline: {os.path.basename(IN)}\n")
    p(f"- API {api}; {len(flat)} actions; {len(tex)} textures\n")

    passes = []
    cur = None
    for a, path in flat:
        f = a.flags
        if f & (rd.ActionFlags.Copy | rd.ActionFlags.Resolve):
            kind = "resolve" if f & rd.ActionFlags.Resolve else "copy"
            passes.append({"kind": kind, "first": a.eventId, "last": a.eventId, "path": path,
                           "src": tname(a.copySource), "dst": tname(a.copyDestination)})
            cur = None
            continue
        if f & rd.ActionFlags.Present:
            passes.append({"kind": "present", "first": a.eventId, "last": a.eventId, "path": path})
            cur = None
            continue
        if f & rd.ActionFlags.Clear:
            continue
        outs = tuple(int(o) for o in a.outputs if int(o))
        key = (outs, int(a.depthOut), bool(f & rd.ActionFlags.Dispatch))
        if cur is not None and cur["key"] == key:
            cur["last"] = a.eventId
            cur["n"] += 1
            cur["instances"] = max(cur["instances"], a.numInstances)
            continue
        cur = {"kind": "dispatch" if key[2] else "draw", "key": key, "first": a.eventId, "last": a.eventId, "n": 1,
               "path": path, "outs": [tname(o) for o in outs], "depth": tname(a.depthOut), "vp": None,
               "instances": a.numInstances}
        if not key[2]:
            try:
                ctl.SetFrameEvent(a.eventId, False)
                v = ctl.GetPipelineState().GetViewport(0)
                cur["vp"] = f"{v.x:.0f},{v.y:.0f} {v.width:.0f}x{abs(v.height):.0f}"
            except Exception:
                pass
        passes.append(cur)

    p("## Passes in order\n")
    by_target = collections.Counter()
    for q in passes:
        marker = " / ".join(x for x in q["path"][-3:] if x)
        head = f"- **{q['first']}-{q['last']}** {q['kind']}"
        if q["kind"] in ("copy", "resolve"):
            p(f"{head}: {q['src']} -> {q['dst']}" + (f"  _({marker})_" if marker else ""))
        elif q["kind"] == "present":
            p(head)
        else:
            tgt = ", ".join(q["outs"]) or "-"
            p(f"{head} x{q['n']}" + (f" (instances up to {q['instances']})" if q["instances"] > 1 else "") +
              f": colour {tgt}; depth {q['depth'] or '-'}" + (f"; viewport {q['vp']}" if q["vp"] else "") +
              (f"  _({marker})_" if marker else ""))
            for o in q["outs"]:
                by_target[o] += q["n"]
            if q["depth"]:
                by_target[q["depth"]] += q["n"]

    p("\n## Targets by draws into them\n")
    for t, n in by_target.most_common(60):
        p(f"- {t}: {n}")
    ctl.Shutdown()
    cap.Shutdown()
    p("\ndone")


try:
    run()
except Exception:
    p(traceback.format_exc())
out.close()
os._exit(0)
