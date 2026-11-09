"""The shaders a draw runs, disassembled: its vertex and pixel shaders' bytecode as RenderDoc lists it, with their
input/output signatures and resource bindings (to see which coordinates sample which texture).

Runs inside RenderDoc's UI process (D3D11; any API RenderDoc disassembles):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<report.md>'; $env:RDC_EVENTS = '24462'
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\shader_disasm.py
RDC_EVENTS: event ids (comma separated); RDC_STAGES: stages to print (default 'vs,ps').
"""
import os
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
EVENTS = [int(e) for e in os.environ.get("RDC_EVENTS", "").split(",") if e.strip()]
STAGES = os.environ.get("RDC_STAGES", "vs,ps").split(",")
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
    stage_of = {"vs": rd.ShaderStage.Vertex, "ps": rd.ShaderStage.Pixel, "cs": rd.ShaderStage.Compute,
                "gs": rd.ShaderStage.Geometry}
    targets = ctl.GetDisassemblyTargets(True)
    p(f"# Shaders: {os.path.basename(IN)} events {','.join(str(e) for e in EVENTS)}")
    p(f"(disassembly target: {targets[0]})")
    for e in EVENTS:
        ctl.SetFrameEvent(e, True)
        ps = ctl.GetPipelineState()
        for name in STAGES:
            stage = stage_of[name.strip()]
            refl = ps.GetShaderReflection(stage)
            if refl is None:
                continue
            p(f"\n## {e} {name.strip().upper()} {int(ps.GetShader(stage))} {refl.entryPoint}")
            p("inputs: " + ", ".join(f"{s.semanticIdxName or s.semanticName} r{s.regIndex}.{s.regChannelMask:x}"
                                      for s in refl.inputSignature))
            p("resources: " + ", ".join(f"{r.name} t{r.fixedBindNumber}" for r in refl.readOnlyResources))
            p("```")
            p(ctl.DisassembleShader(ps.GetGraphicsPipelineObject(), refl, targets[0]))
            p("```")
    ctl.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()


try:
    run()
except Exception:
    p(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
