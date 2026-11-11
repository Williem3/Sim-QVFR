"""The world matrices one eye pass draws with: how many objects would need their own motion vectors.

Runs inside RenderDoc's UI process (D3D11):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<report.md>'; $env:RDC_EYE = '<first>-<last>'
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\world_census.py
RDC_EYE: one eye pass's event range (pass_outline.py finds it). For every draw whose vertex shader declares a
`worldMatrix` constant: the matrix, its distance from the eye (`worldEye`), the targets. The report groups draws by
matrix (one group = one rigid object, or the static world when the matrix is the identity), per pass, with the groups
near the eye listed (the player's car and cockpit) and how the groups sit in the draw order (runs).
"""
import collections
import math
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
FIRST, LAST = (int(x) for x in os.environ["RDC_EYE"].split("-"))
NEAR = float(os.environ.get("RDC_NEAR", "4"))  # metres from the eye: the player's car
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


IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)


def kind(m):
    if all(abs(a - b) < 1e-6 for a, b in zip(m, IDENTITY)):
        return "identity"
    rot = [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]]
    if all(abs(a - b) < 1e-5 for a, b in zip(rot, (1, 0, 0, 0, 1, 0, 0, 0, 1))):
        return "translation"
    return "rotated"


def translation(m):
    row = (m[12], m[13], m[14])
    col = (m[3], m[7], m[11])
    return row if sum(abs(x) for x in row) >= sum(abs(x) for x in col) else col


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

    def tname(rid):
        t = tex.get(int(rid))
        n = names.get(int(rid), f"Resource {int(rid)}")
        return f"{n} {t.width}x{t.height}" + (f" x{t.msSamp}" if t.msSamp > 1 else "") if t else n

    draws = []

    def walk(a):
        if a.flags & rd.ActionFlags.Drawcall and FIRST <= a.eventId <= LAST:
            draws.append(a)
        for c in a.children:
            walk(c)

    for r in ctl.GetRootActions():
        walk(r)

    layouts = {}  # VS id -> (cb index, worldMatrix offset, worldEye offset or None) or None
    rows = []     # (event, pass key, VS id, matrix or None, eye or None, indices)
    for a in draws:
        ctl.SetFrameEvent(a.eventId, False)
        pipe = ctl.GetPipelineState()
        vs = int(pipe.GetShader(rd.ShaderStage.Vertex))
        key = (tuple(int(o) for o in a.outputs if int(o)), int(a.depthOut))
        if vs not in layouts:
            found = None
            refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex) if vs else None
            for i, cb in enumerate(refl.constantBlocks if refl else []):
                offs = {v.name: v.byteOffset for v in cb.variables}
                if "worldMatrix" in offs:
                    found = (i, offs["worldMatrix"], offs.get("worldEye"))
            layouts[vs] = found
        lay = layouts[vs]
        m = eye = None
        if lay:
            rid, off, size = rid_of(pipe.GetConstantBlock(rd.ShaderStage.Vertex, lay[0], 0))
            if int(rid) and size:
                data = ctl.GetBufferData(rid, off, min(size, 512))
                if len(data) >= lay[1] + 64:
                    m = struct.unpack_from("<16f", data, lay[1])
                if lay[2] is not None and len(data) >= lay[2] + 12:
                    eye = struct.unpack_from("<3f", data, lay[2])
        rows.append((a.eventId, key, vs, m, eye, a.numIndices))

    p(f"# World-matrix census: {os.path.basename(IN)}, events {FIRST}-{LAST}\n")
    with_m = [r for r in rows if r[3]]
    p(f"- {len(rows)} draws, {len(with_m)} with a worldMatrix, {len(rows) - len(with_m)} without "
      f"(full-screen and UI draws); {sum(1 for v in layouts.values() if v)} of {len(layouts)} vertex shaders declare it\n")

    eyes = collections.Counter(tuple(round(x, 2) for x in r[4]) for r in with_m if r[4])
    eye = eyes.most_common(1)[0][0] if eyes else None
    p(f"- worldEye: {eye} ({len(eyes)} distinct values)\n")

    def dist(m):
        if eye is None:
            return float("nan")
        t = translation(m)
        return math.sqrt(sum((a - b) ** 2 for a, b in zip(t, eye)))

    passes = collections.OrderedDict()
    for r in rows:
        passes.setdefault(r[1], []).append(r)
    p("## Per pass\n")
    p("| First event | Targets | Draws | With worldMatrix | Distinct matrices | Identity draws | Translation-only | Rotated |"
      f" Draws within {NEAR:g} m of the eye | Runs (same matrix in a row) |")
    p("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for key, rs in passes.items():
        ms = [r[3] for r in rs if r[3]]
        kinds = collections.Counter(kind(m) for m in ms)
        runs = sum(1 for i, m in enumerate(ms) if i == 0 or m != ms[i - 1])
        near = sum(1 for m in ms if kind(m) != "identity" and dist(m) < NEAR)
        tg = ", ".join(tname(o) for o in key[0]) or "-"
        p(f"| {rs[0][0]} | {tg} + depth {tname(key[1]) if key[1] else '-'} | {len(rs)} | {len(ms)} | {len(set(ms))} |"
          f" {kinds['identity']} | {kinds['translation']} | {kinds['rotated']} | {near} | {runs} |")

    groups = collections.OrderedDict()
    for r in with_m:
        groups.setdefault(r[3], []).append(r)
    p(f"\n## Matrices ({len(groups)} distinct over the whole eye pass)\n")
    sizes = collections.Counter(len(g) for g in groups.values())
    p("- draws per matrix: " + ", ".join(f"{n} draws x {c}" for n, c in sorted(sizes.items())) + "\n")
    bands = collections.Counter()
    for m, g in groups.items():
        if kind(m) == "identity":
            bands["identity"] += len(g)
            continue
        d = dist(m)
        band = f"< {NEAR:g} m" if d < NEAR else "< 50 m" if d < 50 else "< 500 m" if d < 500 else ">= 500 m"
        bands[band] += len(g)
    p("- draws by the matrix's distance from the eye: " + ", ".join(f"{k}: {v}" for k, v in bands.items()) + "\n")

    p("### Largest groups and every group near the eye\n")
    p("| Kind | Distance (m) | Draws | Indices | Events | Vertex shaders |")
    p("| --- | --- | --- | --- | --- | --- |")
    listed = sorted(groups.items(), key=lambda kv: -len(kv[1]))
    shown = 0
    for m, g in listed:
        k = kind(m)
        d = dist(m)
        if shown >= 25 and not (k != "identity" and d < NEAR):
            continue
        shown += 1
        ev = [r[0] for r in g]
        vss = collections.Counter(names.get(r[2], str(r[2])) for r in g)
        p(f"| {k} | {'-' if k == 'identity' else f'{d:.1f}'} | {len(g)} | {sum(r[5] for r in g)} | {min(ev)}-{max(ev)} |"
          f" {', '.join(f'{n} x{c}' for n, c in vss.most_common(4))} |")
    ctl.Shutdown()
    cap.Shutdown()
    p("\ndone")


try:
    run()
except Exception:
    p(traceback.format_exc())
out.close()
os._exit(0)
