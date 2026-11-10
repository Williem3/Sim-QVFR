"""The shaders a draw runs as bytecode files, with their constant buffers' bytes: for offline runs of a game's own shaders
(tests/lmu_focus_post_test.cpp [folder]). The game's code: write them to a local folder, never into the repository.

Runs inside RenderDoc's UI process (D3D11):
    $env:RDC_IN = '<capture.rdc>'; $env:RDC_OUT = '<folder>'; $env:RDC_EVENTS = 'copy=26745,composite=27158'
    & 'C:\\Program Files\\RenderDoc\\qrenderdoc.exe' --python tools\\rdc\\shader_bytes.py
RDC_EVENTS: name=event pairs. Each event writes <name>-vs.dxbc, <name>-ps.dxbc and <name>-ps-cb<N>.bin (the bound range
of each pixel shader constant buffer), and a line per file in <folder>\\shaders.txt: signatures, the DXBC checksum as
RenderDoc's shader hash shows it, each buffer's bind offset (non-zero: bound with an offset, PSSetConstantBuffers1).
"""
import os
import struct
import traceback

import renderdoc as rd

IN = os.environ["RDC_IN"]
OUT = os.environ["RDC_OUT"]
EVENTS = [(kv.split("=")[0].strip(), int(kv.split("=")[1])) for kv in os.environ.get("RDC_EVENTS", "").split(",")
          if "=" in kv]
os.makedirs(OUT, exist_ok=True)
out = open(os.path.join(OUT, "shaders.txt"), "w", encoding="utf-8")


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


def checksum(code):
    if len(code) < 20 or code[:4] != b"DXBC":
        return "not DXBC"
    return "-".join(f"{w:08x}" for w in struct.unpack_from("<4I", code, 4))


def signature(elements):
    return ", ".join(f"{s.semanticIdxName or s.semanticName} r{s.regIndex}.{s.regChannelMask:x}" for s in elements)


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
    p(f"# {os.path.basename(IN)}")
    for name, e in EVENTS:
        ctl.SetFrameEvent(e, True)
        pipe = ctl.GetPipelineState()
        for tag, stage in (("vs", rd.ShaderStage.Vertex), ("ps", rd.ShaderStage.Pixel)):
            refl = pipe.GetShaderReflection(stage)
            if refl is None:
                p(f"{name} {e} {tag}: none")
                continue
            code = bytes(refl.rawBytes)
            with open(os.path.join(OUT, f"{name}-{tag}.dxbc"), "wb") as f:
                f.write(code)
            p(f"{name} {e} {tag}: {len(code)} bytes, checksum {checksum(code)}; in: {signature(refl.inputSignature)}; "
              f"out: {signature(refl.outputSignature)}")
            if tag != "ps":
                continue
            for i, cb in enumerate(refl.constantBlocks):
                rid, off, size = rid_of(pipe.GetConstantBlock(stage, i, 0))
                if rid is None or not int(rid):
                    p(f"{name} {e} ps cb{i} {cb.name}: not bound")
                    continue
                data = bytes(ctl.GetBufferData(rid, off, size))
                with open(os.path.join(OUT, f"{name}-ps-cb{i}.bin"), "wb") as f:
                    f.write(data)
                p(f"{name} {e} ps cb{i} {cb.name}: {len(data)} bytes, bound at offset {off} (size {size}), "
                  f"{cb.byteSize} used by the shader")
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
