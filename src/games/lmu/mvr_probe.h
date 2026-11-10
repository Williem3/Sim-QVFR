// LMU's MVR probe, log only (gate B of docs/recon-lmu.md "Multi-view rendering"; [experiment] lmu_mvr_probe=1): NVAPI's
// single-pass stereo support on the game's D3D11 device, and every vertex shader created on it converted to draw a
// second view (engine/dxbc_patch.h) and created through NVAPI with the custom semantics, then released. Nothing the
// game draws changes. Under RenderDoc (renderdoc.dll loaded: it wraps the device, which NVAPI's extensions can't
// see) the NVAPI creation is skipped and only the conversions are counted. lmu_mvr_dump=1 also saves every vertex
// shader's bytecode in logs\shaders (the game's code: local, never in the repository). With lmu_mvr=1 (gate C) the
// converted shader isn't released: each game shader carries it as private data (kMvrTwinGuid), one NVAPI
// shader per distinct bytecode.
//
// Two ways to the two views (MvrBackend): NVIDIA's single-pass stereo (RTX 20 series and newer) or Sim-QVFR's own,
// instancing (dxbc::add_instanced_view: every draw drawn twice as instances, the vertex shader picking the viewport;
// any GPU whose D3D11 driver lets a vertex shader do that, D3D11_OPTIONS3, which RenderDoc passes on too). With twins
// attached, every game shader also carries its instanced twin (kMvrInstancedTwinGuid) where the device can, so a
// session on NVIDIA's falls back to instancing if the driver ever refuses it; input layouts with per-instance data
// carry a twin with their step rates doubled (kMvrInstancedLayoutGuid). [experiment] multi_view_software=1 uses
// instancing even where NVIDIA's is there (its test on an NVIDIA card).
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
    uint64_t instancedConverted = 0, instancedCreated = 0, instancedFailed = 0;  // add_instanced_view's, by D3D11
    uint64_t layouts = 0, instancedLayouts = 0;  // input layouts created; those with per-instance data, twinned
    uint64_t dumped = 0;
    uint64_t repeats = 0;       // created again with a bytecode seen before (its twin reused)
    uint64_t attached = 0;      // game shaders given their twin (lmu_mvr)
    uint64_t yebisCopies = 0, yebisComposites = 0;  // YEBIS's copy and composite pixel shaders marked (yebis_shaders.h)
    uint64_t finalBlits = 0;  // the game's final blit pixel shaders marked (yebis_shaders.h)
    uint64_t lensMasks = 0, vruiPanels = 0;  // the lens mask's and VR UI panel's vertex shaders marked (VsRole)
    double convertMs = 0, maxConvertMs = 0, nvapiMs = 0, instancedMs = 0;
    std::map<std::string, uint64_t> refused;           // why add_second_view didn't convert a shader, and how often
    std::map<std::string, uint64_t> instancedRefused;  // add_instanced_view's
};

// The private data a game vertex shader carries its converted twins under (an ID3D11VertexShader each): NVIDIA's
// single-pass stereo's and the instanced one.
inline constexpr GUID kMvrTwinGuid = {0x6d1c4a52, 0x3b8e, 0x4f61, {0x9a, 0x27, 0x5e, 0x10, 0xc3, 0x7b, 0x44, 0xd9}};
inline constexpr GUID kMvrInstancedTwinGuid = {0x2f7c9e14, 0x58a1, 0x4b3d, {0x8e, 0x06, 0xc1, 0x9d, 0x4a, 0x72, 0x35, 0xb0}};
// An input layout with per-instance data: its twin with every per-instance step rate doubled (an ID3D11InputLayout).
inline constexpr GUID kMvrInstancedLayoutGuid = {0x91e3b6d0, 0x7c42, 0x4e85, {0xa3, 0x5f, 0x0d, 0x28, 0xe6, 0x19, 0xbc, 0x47}};

// How the eye passes draw their focus views: none (four passes), NVIDIA's single-pass stereo, or instancing.
enum class MvrBackend { none, nvapi, instanced };
const char* mvr_backend_name(MvrBackend backend);

// xrCreateInstance: hooks D3D11 device creation (until mvr_probe_device), so the shaders the game creates before its
// OpenXR session are seen. The device's shader creation is an entry of a vtable copy it points at (object_vtable.h).
// `attachTwins` (lmu_mvr=1): the converted shaders are kept and attached to the game's.
bool mvr_probe_install(const wchar_t* ini, bool attachTwins = false, bool nameShaders = false);
// xrCreateSession with the game's device: device creation unhooked; NVAPI's flags on the device; its shader creation
// hooked if device creation wasn't.
void mvr_probe_device(ID3D11Device* device);
// Every xrEndFrame: a summary when the counts changed (at most every 5 s).
void mvr_probe_frame_end();
MvrProbeStats mvr_probe_stats();
// The game's device can draw two views (NVAPI's single-pass stereo with XYZW, or a vertex shader picking the viewport,
// seen at its first vertex shader or at xrCreateSession) and the probe attaches the converted shaders: MVR can run.
// False before either, or with the probe off.
bool mvr_probe_usable();
// The way it draws them (NVIDIA's where the device has it, unless multi_view_software=1; else instancing); none when
// not usable.
MvrBackend mvr_probe_backend();
// Instanced twins are attached (the device can and the probe attaches): the fallback when NVIDIA's is refused.
bool mvr_probe_instanced();
// Input layouts with per-instance data seen (each with its twin): a doubled draw then looks at the bound layout.
uint64_t mvr_probe_instanced_layouts();
// Counts the vertex shaders created on the hooked device (any thread): a shader looked up by its address before may be
// a new one since (mvr_pass.cpp's twin cache).
uint64_t mvr_shader_generation();

}  // namespace lmu
