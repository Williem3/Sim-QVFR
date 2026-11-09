"""What a per-eye engine's eye passes draw, shader by shader: the groundwork for drawing several views in one pass.

Runs inside RenderDoc's UI process (D3D11 or D3D12):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<report.md>'; $env:RDC_EYES = '<first>-<last>,<first>-<last>'
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\view_census.py
RDC_EYES: each eye pass's event range (pass_outline.py finds them); a view drawn in several spans (ACC: each pass draws
the eye pair, then the focus pair, into the same targets; viewport_runs.py finds them) joins them with +, as
6700-8326+11032-14569. For each eye, per pass (a run of draws into the same
targets): draws, instanced draws, distinct vertex and pixel shaders, hull/domain/geometry stages, pixel shaders that read
the screen position (SV_Position) or sample a depth target. Then the vertex shaders' outputs, the constant buffers both
eyes bind at each pass's first draw with every value that differs between the eyes (named from the shader reflection
when it has names), and the API calls each eye makes. RDC_DISASM (optional): a folder for the busiest shaders' text;
RDC_BYTECODE (optional): a folder for every vertex shader's bytecode (the game's own code: keep it out of the repository).
"""
import collections
import os
import re
import struct
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
EYES = [[tuple(int(x) for x in span.split("-")) for span in r.split("+")] for r in os.environ["RDC_EYES"].split(",")]


def inside(event, spans):
    return any(first <= event <= last for first, last in spans)


def spans_text(spans):
    return "+".join(f"{first}-{last}" for first, last in spans)
DISASM = os.environ.get("RDC_DISASM", "")
BYTECODE = os.environ.get("RDC_BYTECODE", "")  # optional: a folder for every vertex shader's bytecode
out = open(OUT, "w", encoding="utf-8")

STAGES = [("VS", rd.ShaderStage.Vertex), ("HS", rd.ShaderStage.Hull), ("DS", rd.ShaderStage.Domain),
          ("GS", rd.ShaderStage.Geometry), ("PS", rd.ShaderStage.Pixel)]


def p(*a):
    out.write(" ".join(str(x) for x in a) + "\n")
    out.flush()


def rid_of(u):
    """A bound descriptor's resource and range, across RenderDoc's old and new descriptor types."""
    d = getattr(u, "descriptor", u)
    rid = getattr(d, "resource", None)
    if rid is None:
        rid = getattr(d, "resourceId", None)
    return rid, int(getattr(d, "byteOffset", 0)), int(getattr(d, "byteSize", 0))


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
    sf = ctl.GetStructuredFile()
    targets = ctl.GetDisassemblyTargets(True)

    def tname(rid):
        i = int(rid)
        if i == 0:
            return "-"
        t = tex.get(i)
        n = names.get(i, f"Resource {i}")
        return f"{n} {t.width}x{t.height}" + (f" x{t.msSamp}" if t and t.msSamp > 1 else "") if t else n

    draws, all_events = [], []

    def walk(a):
        for e in a.events:
            all_events.append((e.eventId, e.chunkIndex))
        if a.flags & rd.ActionFlags.Drawcall:
            draws.append(a)
        for c in a.children:
            walk(c)

    for r in ctl.GetRootActions():
        walk(r)
    def eye_depths(spans):  # the depth targets an eye draws into itself (not the shadow map made before it)
        return {int(a.depthOut) for a in draws if inside(a.eventId, spans) and int(a.depthOut)}

    shaders = {}  # id -> info
    p(f"# View census: {os.path.basename(IN)}\n")
    p(f"- {len(draws)} draws in the frame; eyes {', '.join(spans_text(s) for s in EYES)}; disassembly target {targets[0] if targets else '-'}\n")

    eyes = []
    for spans in EYES:
        passes, cur = [], None
        depth_ids = eye_depths(spans)
        for a in draws:
            if not inside(a.eventId, spans):
                continue
            key = (tuple(int(o) for o in a.outputs if int(o)), int(a.depthOut))
            if cur is None or cur["key"] != key:
                cur = {"key": key, "first": a.eventId, "draws": 0, "instanced": 0, "vs": collections.Counter(),
                       "ps": collections.Counter(), "stages": collections.Counter(), "pos_ps": 0, "depth_ps": 0,
                       "cbs": None, "vp": None}
                passes.append(cur)
            cur["draws"] += 1
            cur["instanced"] += 1 if a.numInstances > 1 else 0
            ctl.SetFrameEvent(a.eventId, False)
            pipe = ctl.GetPipelineState()
            ids = {}
            for sname, stage in STAGES:
                sid = int(pipe.GetShader(stage))
                ids[sname] = sid
                if not sid:
                    continue
                cur["stages"][sname] += 1
                if sid not in shaders:
                    refl = pipe.GetShaderReflection(stage)
                    info = {"stage": sname, "uses": 0, "refl": refl, "name": names.get(sid, str(sid)), "text": None,
                            "outs": [], "ins": [], "cbvars": []}
                    try:
                        info["outs"] = [f"{s.semanticName}{s.semanticIndex}" for s in refl.outputSignature]
                        info["ins"] = [f"{s.semanticName}{s.semanticIndex}" for s in refl.inputSignature]
                        info["reads_xy"] = any(s.systemValue == rd.ShaderBuiltin.Position and
                                               (s.channelUsedMask & 3) for s in refl.inputSignature)
                        for cb in refl.constantBlocks:
                            info["cbvars"].append((cb.name, [(v.name, v.byteOffset) for v in cb.variables]))
                    except Exception as e:
                        info["err"] = repr(e)
                    try:
                        info["text"] = ctl.DisassembleShader(pipe.GetGraphicsPipelineObject(), refl, targets[0])
                    except Exception as e:
                        info["text"] = f"(disassembly failed: {e!r})"
                    shaders[sid] = info
                shaders[sid]["uses"] += 1
            cur["vs"][ids["VS"]] += 1
            cur["ps"][ids["PS"]] += 1
            ps = shaders.get(ids["PS"])
            if ps and ps.get("reads_xy"):
                cur["pos_ps"] += 1
            if ids["PS"]:
                try:
                    for u in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel):
                        if int(rid_of(u)[0]) in depth_ids:
                            cur["depth_ps"] += 1
                            break
                except Exception:
                    pass
            if cur["cbs"] is None:  # the pass's first draw: every bound constant buffer's contents
                cur["cbs"] = {}
                try:
                    v = pipe.GetViewport(0)
                    cur["vp"] = f"{v.x:.0f},{v.y:.0f} {v.width:.0f}x{abs(v.height):.0f}"
                except Exception:
                    pass
                for sname, stage in STAGES:
                    if not ids[sname]:
                        continue
                    refl = shaders[ids[sname]]["refl"]
                    for i, cb in enumerate(refl.constantBlocks):
                        try:
                            rid, off, size = rid_of(pipe.GetConstantBlock(stage, i, 0))
                            if int(rid) and size:
                                data = ctl.GetBufferData(rid, off, min(size, 4096))
                                cur["cbs"][(sname, cb.name, i)] = (int(rid), data,
                                                                   [(v.name, v.byteOffset) for v in cb.variables])
                        except Exception as e:
                            cur["cbs"][(sname, cb.name, i)] = (0, b"", [("error " + repr(e), 0)])
        calls = collections.Counter(sf.chunks[c].name for e, c in all_events if inside(e, spans))
        eyes.append({"passes": passes, "calls": calls})

    for k, eye in enumerate(eyes):
        p(f"## Eye {k} (events {spans_text(EYES[k])})\n")
        p("| First event | Targets | Viewport | Draws | Instanced | VS | PS | HS/DS/GS draws | PS reading SV_Position.xy | PS sampling the eye's own depth |")
        p("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for q in eye["passes"]:
            tg = ", ".join(tname(o) for o in q["key"][0]) + f"; depth {tname(q['key'][1])}"
            hdg = "/".join(str(q["stages"][s]) for s in ("HS", "DS", "GS"))
            p(f"| {q['first']} | {tg} | {q['vp']} | {q['draws']} | {q['instanced']} | {len(q['vs'])} | {len(q['ps'])} | "
              f"{hdg} | {q['pos_ps']} | {q['depth_ps']} |")
        p("")

    p("## Constant buffers that differ between the eyes (each pass's first draw)\n")
    if len(eyes) >= 2:
        pairs = []  # the eyes' passes paired by their targets, in order (an eye can skip a pass the other has)
        for key in dict.fromkeys(q["key"] for q in eyes[0]["passes"]):
            pairs += zip([q for q in eyes[0]["passes"] if q["key"] == key], [q for q in eyes[1]["passes"] if q["key"] == key])
        for qa, qb in sorted(pairs, key=lambda x: x[0]["first"]):
            if not qa["cbs"] or not qb["cbs"]:
                continue
            for key, (rida, da, vars_) in qa["cbs"].items():
                if key not in qb["cbs"]:
                    continue
                ridb, db, _ = qb["cbs"][key]
                n = min(len(da), len(db)) // 4
                fa, fb = struct.unpack(f"<{n}f", da[:n * 4]), struct.unpack(f"<{n}f", db[:n * 4])
                diff = [i for i in range(n) if fa[i] != fb[i] and not (fa[i] != fa[i] and fb[i] != fb[i])]
                if not diff:
                    continue
                offs = sorted(vars_, key=lambda v: v[1])

                def var_at(byte):
                    name = "?"
                    for vn, vo in offs:
                        if vo <= byte:
                            name = f"{vn}+{byte - vo}" if byte > vo else vn
                    return name

                p(f"- pass {qa['first']}/{qb['first']} {key[0]} cb `{key[1]}` slot {key[2]} (buffer {rida}/{ridb}, "
                  f"{n * 4} bytes): {len(diff)} floats differ")
                for i in diff[:40]:
                    p(f"  - +{i * 4} `{var_at(i * 4)}`: {fa[i]:.6g} / {fb[i]:.6g}")
        p("")

    p("## Shaders\n")
    for sname in ("VS", "HS", "DS", "GS", "PS"):
        group = [(sid, s) for sid, s in shaders.items() if s["stage"] == sname]
        if not group:
            continue
        p(f"### {sname}: {len(group)} distinct, {sum(s['uses'] for _, s in group)} draws\n")
        sigs = collections.Counter(" ".join(s["outs"] if sname != "PS" else s["ins"]) for _, s in group)
        p(f"{'Output' if sname != 'PS' else 'Input'} signatures:\n")
        for sig, n in sigs.most_common(25):
            p(f"- {n}x `{sig}`")
        cbnames = collections.Counter()
        for _, s in group:
            for cbn, vs in s["cbvars"]:
                cbnames[cbn] += 1
        p("\nConstant buffers (by name, shaders using them):\n")
        for cbn, n in cbnames.most_common(25):
            sample = next((vs for _, s in group for c, vs in s["cbvars"] if c == cbn), [])
            p(f"- {n}x `{cbn}`: " + ", ".join(f"{v}@{o}" for v, o in sample[:24]))
        if sname in ("VS", "DS", "GS"):
            o0 = collections.Counter()
            for _, s in group:
                t = s["text"] or ""
                o0[len(re.findall(r"^\s*\d*:?\s*\w+(?:_\w+)*\s+o0(?:\.\w+)?\s*,", t, re.M))] += 1
            p("\nInstructions writing o0 per shader: " + ", ".join(f"{k}: {v}" for k, v in sorted(o0.items())))
        p("")
        if DISASM:
            os.makedirs(DISASM, exist_ok=True)
            for sid, s in sorted(group, key=lambda x: -x[1]["uses"])[:6]:
                with open(os.path.join(DISASM, f"{sname}_{sid}_{s['uses']}.txt"), "w", encoding="utf-8") as f:
                    f.write(s["text"] or "")
        if BYTECODE and sname == "VS":  # every vertex shader's DXBC, for offline conversion tests (the game's code:
            os.makedirs(BYTECODE, exist_ok=True)  # keep it local, never in the repository)
            for sid, s in group:
                try:
                    with open(os.path.join(BYTECODE, f"VS_{sid}_{s['uses']}.dxbc"), "wb") as f:
                        f.write(bytes(s["refl"].rawBytes))
                except Exception as e:
                    p(f"- bytecode of VS {sid}: {e!r}")

    p("## API calls per eye\n")
    keys = sorted(set().union(*(e["calls"].keys() for e in eyes)), key=lambda k: -eyes[0]["calls"].get(k, 0))
    p("| Call | " + " | ".join(f"eye {k}" for k in range(len(eyes))) + " |")
    p("| --- | " + " | ".join("---" for _ in eyes) + " |")
    for k in keys[:45]:
        p(f"| {k} | " + " | ".join(str(e["calls"].get(k, 0)) for e in eyes) + " |")
    ctl.Shutdown()
    cap.Shutdown()
    p("\ndone")


try:
    run()
except Exception:
    p(traceback.format_exc())
out.close()
os._exit(0)
