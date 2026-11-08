// LMU's MVR probe, log only (gate B of docs/recon-lmu.md "Multi-view rendering"; [experiment] lmu_mvr_probe=1): NVAPI's
// single-pass stereo support on the game's D3D11 device, and every vertex shader created on it converted to draw a
// second view (engine/dxbc_patch.h) and created through NVAPI with the custom semantics, then released. Nothing the
// game draws changes. Under RenderDoc (renderdoc.dll loaded: it wraps the device, which NVAPI's extensions can't
// see) the NVAPI creation is skipped and only the conversions are counted. lmu_mvr_dump=1 also saves every vertex
// shader's bytecode in logs\shaders (the game's code: local, never in the repository). With lmu_mvr=1 (gate C) the
// converted shader isn't released: each game shader carries it as private data (kMvrTwinGuid), one NVAPI
// shader per distinct bytecode.
#pragma once

#include <guiddef.h>

#include <cstdint>
#include <map>
#include <string>

struct ID3D11Device;
struct ID3D11VertexShader;

namespace lmu {

struct MvrProbeStats {
    uint64_t created = 0;       // vertex shaders created on a hooked device
    uint64_t converted = 0;     // add_second_view converted them
    uint64_t nvapiCreated = 0;  // and NVAPI created the converted shader
    uint64_t nvapiFailed = 0;
    uint64_t dumped = 0;
    uint64_t repeats = 0;       // created again with a bytecode seen before (its twin reused)
    uint64_t attached = 0;      // game shaders given their twin (lmu_mvr)
    double convertMs = 0, maxConvertMs = 0, nvapiMs = 0;
    std::map<std::string, uint64_t> refused;  // why a shader wasn't converted, and how often
};

// The private data a game vertex shader carries its converted twin under (an ID3D11VertexShader).
inline constexpr GUID kMvrTwinGuid = {0x6d1c4a52, 0x3b8e, 0x4f61, {0x9a, 0x27, 0x5e, 0x10, 0xc3, 0x7b, 0x44, 0xd9}};

// xrCreateInstance: hooks D3D11 device creation, so the shaders the game creates before its OpenXR session are seen.
// `attachTwins` (lmu_mvr=1): the converted shaders are kept and attached to the game's.
bool mvr_probe_install(const wchar_t* ini, bool attachTwins = false);
// xrCreateSession with the game's device: NVAPI's flags on it; its shader creation hooked if device creation wasn't.
void mvr_probe_device(ID3D11Device* device);
// Every xrEndFrame: a summary when the counts changed (at most every 5 s).
void mvr_probe_frame_end();
MvrProbeStats mvr_probe_stats();
// Counts the vertex shaders created on the hooked device (any thread): a shader looked up by its address before may be
// a new one since (mvr_pass.cpp's twin cache).
uint64_t mvr_shader_generation();

}  // namespace lmu
