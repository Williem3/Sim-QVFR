"""What chosen draws' vertex shaders output, vertex by vertex: for full-screen passes whose UVs are baked into their
vertices (YEBIS's post-processing), the source region each draw samples.
Runs inside RenderDoc's UI process (any API RenderDoc replays):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<report.md>'; $env:RDC_EVENTS = '19286,30567'
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\vertex_outputs.py
RDC_EVENTS: event ids (comma separated); RDC_VERTICES: how many vertices to print (default 6).
"""
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
EVENTS = [int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()]
VERTICES = int(os.environ.get("RDC_VERTICES", "6"))
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
    p(f"# Vertex outputs: {os.path.basename(IN)}\n")
    for event in EVENTS:
        ctl.SetFrameEvent(event, True)
        state = ctl.GetPipelineState()
        vs = state.GetShaderReflection(rd.ShaderStage.Vertex)
        mesh = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        vp = state.GetViewport(0)
        p(f"## {event}: viewport {vp.x:.0f},{vp.y:.0f} {vp.width:.0f}x{vp.height:.0f}; {mesh.numIndices} vertices, "
          f"stride {mesh.vertexByteStride}")
        if not vs or mesh.vertexResourceId == rd.ResourceId.Null() or not mesh.vertexByteStride:
            p("- no vertex output")
            continue
        data = ctl.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, 0)
        sigs = list(vs.outputSignature)
        # RenderDoc's post-VS data: the position first, then the other outputs in signature order, packed tightly.
        sigs.sort(key=lambda s: 0 if s.systemValue == rd.ShaderBuiltin.Position else 1)
        for v in range(min(VERTICES, mesh.numIndices)):
            off = v * mesh.vertexByteStride
            parts = []
            for i, sig in enumerate(sigs):
                comps = max(1, sig.compCount)
                if off + 4 * comps > len(data):
                    break
                vals = struct.unpack_from(f"{comps}f", data, off)
                off += 4 * comps
                name = sig.semanticIdxName or sig.varName or f"o{i}"
                parts.append(f"{name}=(" + ", ".join(f"{x:.5f}" for x in vals) + ")")
            p(f"- v{v}: " + "  ".join(parts))
    ctl.Shutdown()
    cap.Shutdown()
    p("\ndone")


try:
    run()
except Exception:
    p(traceback.format_exc())
out.close()
os._exit(0)
