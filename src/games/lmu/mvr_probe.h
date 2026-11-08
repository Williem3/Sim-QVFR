// LMU's MVR probe, log only (gate B of docs/recon-lmu.md "Multi-view rendering"; [experiment] lmu_mvr_probe=1): NVAPI's
// single-pass stereo support on the game's D3D11 device, and every vertex shader created on it converted to draw a
// second view (engine/dxbc_patch.h) and created through NVAPI with the custom semantics, then released. Nothing the
// game draws changes. Under RenderDoc (renderdoc.dll loaded: it wraps the device, which NVAPI's extensions can't
// see) the NVAPI creation is skipped and only the conversions are counted. lmu_mvr_dump=1 also saves every vertex
// shader's bytecode in logs\shaders (the game's code: local, never in the repository).
#pragma once

#include <cstdint>
#include <map>
#include <string>

struct ID3D11Device;

namespace lmu {

struct MvrProbeStats {
    uint64_t created = 0;       // vertex shaders created on a hooked device
    uint64_t converted = 0;     // add_second_view converted them
    uint64_t nvapiCreated = 0;  // and NVAPI created the converted shader
    uint64_t nvapiFailed = 0;
    uint64_t dumped = 0;
    double convertMs = 0, maxConvertMs = 0, nvapiMs = 0;
    std::map<std::string, uint64_t> refused;  // why a shader wasn't converted, and how often
};

// xrCreateInstance: hooks D3D11 device creation, so the shaders the game creates before its OpenXR session are seen.
bool mvr_probe_install(const wchar_t* ini);
// xrCreateSession with the game's device: NVAPI's flags on it; its shader creation hooked if device creation wasn't.
void mvr_probe_device(ID3D11Device* device);
// Every xrEndFrame: a summary when the counts changed (at most every 5 s).
void mvr_probe_frame_end();
MvrProbeStats mvr_probe_stats();

}  // namespace lmu
