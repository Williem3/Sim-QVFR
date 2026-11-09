"""What chosen draws read and write: targets, viewport, shaders, every bound shader resource, and the constants.

Runs inside RenderDoc's UI process (D3D11; written for post-processing chains such as YEBIS's glare):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<report.md>'; $env:RDC_EVENTS = '26745-27211,42631'
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\draw_inputs.py
RDC_EVENTS: event ids or ranges; every draw or dispatch inside them is listed. Optional:
- RDC_CONSTANTS=0 leaves out the constant buffers (they are listed per stage, variable by variable, by default).
- RDC_SAVE='<event>:<resource id>:<file>;...' saves that texture (mip 0, slice 0) as it is after that event, to
  <file> (.dds keeps the texture's own format, readable with tools/dds_read.py; .exr HDR; .png 8-bit). Resource ids
  are the numbers the report prints (`Render Target 4151`).
"""
import os
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
EVENTS = os.environ.get("RDC_EVENTS", "")
CONSTANTS = os.environ.get("RDC_CONSTANTS", "1") != "0"
SAVE = os.environ.get("RDC_SAVE", "")
out = open(OUT, "w", encoding="utf-8")


def p(*a):
    out.write(" ".join(str(x) for x in a) + "\n")
    out.flush()


def ranges(spec):
    r = []
    for part in spec.replace(" ", "").split(","):
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            r.append((int(a), int(b)))
        else:
            r.append((int(part), int(part)))
    return r


def fmt_value(v, n):
    vals = list(v.value.f32v[:n])
    return " ".join(f"{x:.6g}" for x in vals)


def dump_vars(vars_, indent):
    for v in vars_:
        if v.members:
            p(f"{indent}- {v.name}:")
            dump_vars(v.members, indent + "  ")
        else:
            n = max(1, v.rows * v.columns)
            p(f"{indent}- {v.name} = {fmt_value(v, min(n, 16))}")


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
        i = int(rid)
        if i == 0:
            return None
        t = tex.get(i)
        n = names.get(i, f"Resource {i}")
        if not t:
            return n
        f = t.format.Name() if hasattr(t.format, "Name") else str(t.format)
        return f"{n} {t.width}x{t.height} {f}" + (f" x{t.msSamp}" if t.msSamp > 1 else "") + \
            (f" mips {t.mips}" if t.mips > 1 else "")

    wanted = ranges(EVENTS)
    acts = []

    def walk(a):
        if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
            if any(lo <= a.eventId <= hi for lo, hi in wanted):
                acts.append(a)
        for c in a.children:
            walk(c)

    for r in ctl.GetRootActions():
        walk(r)
    p(f"# Draw inputs: {os.path.basename(IN)} events {EVENTS}\n")
    stages = [(rd.ShaderStage.Vertex, "VS"), (rd.ShaderStage.Pixel, "PS"), (rd.ShaderStage.Compute, "CS")]
    for a in acts:
        ctl.SetFrameEvent(a.eventId, True)
        ps = ctl.GetPipelineState()
        outs = [tname(o) for o in a.outputs if int(o)]
        p(f"## {a.eventId} {a.customName or ''}".rstrip())
        p(f"- targets: {', '.join(outs) or '-'}; depth {tname(a.depthOut) or '-'}")
        if a.flags & rd.ActionFlags.Drawcall:  # the API call, as a hook on it sees it
            if a.flags & rd.ActionFlags.Indexed:
                p(f"- call: DrawIndexed{'Instanced' if a.numInstances > 1 else ''}(indices {a.numIndices}, start index "
                  f"{a.indexOffset}, base vertex {a.baseVertex}, instances {a.numInstances})")
            else:
                p(f"- call: Draw{'Instanced' if a.numInstances > 1 else ''}(vertices {a.numIndices}, start vertex "
                  f"{a.vertexOffset}, instances {a.numInstances})")
        try:
            v = ps.GetViewport(0)
            p(f"- viewport {v.x:.1f},{v.y:.1f} {v.width:.1f}x{abs(v.height):.1f}; vertices {a.numIndices}")
        except Exception:
            pass
        for stage, label in stages:
            refl = ps.GetShaderReflection(stage)
            if refl is None:
                continue
            p(f"- {label} {int(ps.GetShader(stage))} {ps.GetShaderEntryPoint(stage)}")
            try:
                ro = ps.GetReadOnlyResources(stage, True)
            except TypeError:
                ro = ps.GetReadOnlyResources(stage)
            for used in ro:
                # RenderDoc 1.2x: UsedDescriptor (access.index, descriptor.resource); older: BoundResourceArray.
                if hasattr(used, "descriptor"):
                    rid = used.descriptor.resource
                    slot = used.access.index
                    sname = refl.readOnlyResources[slot].name if slot < len(refl.readOnlyResources) else "?"
                    p(f"  - t{slot} {sname}: {tname(rid)}")
                else:
                    for res in used.resources:
                        if int(res.resourceId):
                            p(f"  - t{used.bindPoint.bind} : {tname(res.resourceId)}")
            if not CONSTANTS:
                continue
            for i, cb in enumerate(refl.constantBlocks):
                try:
                    bind = ps.GetConstantBlock(stage, i, 0)
                    desc = bind.descriptor if hasattr(bind, "descriptor") else bind
                    buf = desc.resource if hasattr(desc, "resource") else desc.resourceId
                    off = desc.byteOffset
                    size = desc.byteSize
                    vars_ = ctl.GetCBufferVariableContents(ps.GetGraphicsPipelineObject(), ps.GetShader(stage),
                                                           stage, ps.GetShaderEntryPoint(stage), i, buf, off, size)
                    p(f"  - cb {cb.name}:")
                    dump_vars(vars_, "    ")
                except Exception as e:
                    p(f"  - cb {cb.name}: (not read: {e})")
        p("")

    for item in [s for s in SAVE.split(";") if s.strip()]:
        ev, rid, path = item.split(":", 2)
        ctl.SetFrameEvent(int(ev), True)
        target = None
        for t in ctl.GetTextures():
            if int(t.resourceId) == int(rid):
                target = t.resourceId
        if target is None:
            p(f"save {item}: no such texture")
            continue
        s = rd.TextureSave()
        s.resourceId = target
        s.mip = 0
        s.slice.sliceIndex = 0
        s.alpha = rd.AlphaMapping.Preserve
        ext = path.lower().rsplit(".", 1)[-1]
        s.destType = {"exr": rd.FileType.EXR, "dds": rd.FileType.DDS}.get(ext, rd.FileType.PNG)
        r = ctl.SaveTexture(s, path)
        p(f"save {item}: {r}")
    ctl.Shutdown()
    cap.Shutdown()
    p("\ndone")


try:
    run()
except Exception:
    p(traceback.format_exc())
out.close()
os._exit(0)
