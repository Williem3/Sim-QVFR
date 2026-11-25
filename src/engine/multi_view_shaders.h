// The multi-view shader service (LMU's MVR probe at first; the SCS games use it too), log only (gate B of docs/recon-lmu.md "Multi-view rendering"; [experiment] lmu_mvr_probe=1): NVAPI's
// single-pass stereo support on the game's D3D11 device, and every vertex shader created on it converted to draw a
// second view (engine/dxbc_patch.h) and created through NVAPI with the custom semantics, then released. Nothing the
// game draws changes. Under RenderDoc (renderdoc.dll loaded: it wraps the device, which NVAPI's extensions can't
// see) the NVAPI creation is skipped and only the conversions are counted. lmu_mvr_dump=1 also saves every vertex
// shader's bytecode in logs\shaders (the game's code: local, never in the repository). With lmu_mvr=1 (gate C) the
// converted shader isn't released: each game shader carries it as private data (kMvrTwinGuid), one NVAPI
// shader per distinct bytecode.
//
// Two ways to the two views (MvrBackend): NVIDIA's multi-view rendering (RTX 20 series and newer) or Sim-QVFR's own,
// instancing (dxbc::add_instanced_view: every draw drawn twice as instances, the vertex shader picking the viewport;
// any GPU whose D3D11 driver lets a vertex shader do that, D3D11_OPTIONS3, which RenderDoc passes on too). With twins
// attached, every game shader also carries its instanced twin (kMvrInstancedTwinGuid) where the device can, so a
// session on NVIDIA's falls back to instancing if the driver ever refuses it; input layouts with per-instance data
// carry a twin with their step rates doubled (kMvrInstancedLayoutGuid). [experiment] multi_view_software=1 uses
// instancing even where NVIDIA's is there (its test on an NVIDIA card).
#pragma once

#include "multi_view_shaders_abi.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

struct ID3D11Device;
struct ID3D11VertexShader;

namespace simqvfr::mvs {

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
    uint64_t positionTwins = 0;  // full-screen shaders given a position twin (mvr_probe_position_twins)
    uint64_t ownLoadsTwins = 0;  // pixel shaders given an own-pixel-loads twin (mvr_probe_own_pixel_loads)
    // Pixel shaders (lmu_mvr): created, given a mip bias twin (dxbc::add_focus_mip_bias; texture sharpening), and why not.
    uint64_t pixelCreated = 0, pixelTwinned = 0, pixelRepeats = 0, pixelFailed = 0;
    std::map<std::string, uint64_t> pixelRefused;
    // Their slot twins (lmu_car_vectors): made, of a mip bias twin too, not created; why refused.
    uint64_t slotTwinned = 0, slotBiasTwinned = 0, slotFailed = 0;
    uint64_t slotNormals = 0;  // night car lighting: slot twins that write the game's normal too
    uint64_t slotAos = 0;      // and its baked ambient occlusion
    uint64_t slotDetails = 0;  // and the normal its normal maps bent
    std::map<std::string, uint64_t> slotRefused;
    double convertMs = 0, maxConvertMs = 0, nvapiMs = 0, instancedMs = 0;
    std::map<std::string, uint64_t> refused;           // why add_second_view didn't convert a shader, and how often
    std::map<std::string, uint64_t> instancedRefused;  // add_instanced_view's
};

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
// The SCS games: full-screen shaders get their position twins (kFocusPositionTwinGuid) from now on.
void mvr_probe_position_twins(bool on);
// The SCS games: pixel shaders get their own-pixel-loads twins (kOwnPixelLoadsTwinGuid) from now on.
void mvr_probe_own_pixel_loads(bool on);
// The SCS games: vertex and pixel shaders that read a constant row as a colour get kCb0TintRowsGuid from now on.
void mvr_probe_tint_rows(bool on);
// The SCS mirror diagnostic (dev builds): every shader tagged with its id (kShaderIdGuid, DXBC checksum words 0-1)
// and the rows of constant buffer slots 0-3 it reads by a fixed index (kCbRows4Guid: 4 x 64 uint64).
void mvr_probe_cb_rows(bool on);
// Every xrEndFrame: a summary when the counts changed (at most every 5 s).
void mvr_probe_frame_end();
MvrProbeStats mvr_probe_stats();
// YEBIS's copy pixel shaders and the game's final blits marked so far: two counters, no copy of the stats (the passes ask
// at every pass's start and frame's end).
uint64_t mvr_probe_yebis_copies();
uint64_t mvr_probe_final_blits();
// The game's device can draw two views (NVAPI's single-pass stereo with XYZW, or a vertex shader picking the viewport,
// seen at its first vertex shader or at xrCreateSession) and the probe attaches the converted shaders: MVR can run.
// False before either, or with the probe off.
bool mvr_probe_usable();
// The way it draws them (NVIDIA's where the device has it, unless multi_view_software=1; else instancing); none when
// not usable.
MvrBackend mvr_probe_backend();
// Instanced twins are attached (the device can and the probe attaches): the fallback when NVIDIA's is refused.
bool mvr_probe_instanced();
// Input layouts with per-instance data seen with their twin.
uint64_t mvr_probe_instanced_layouts();
// A doubled draw must look at the bound layout (kMvrLayoutKindGuid): a layout with per-instance data was seen (twinned
// or not), or layouts may have been made before the device was hooked (not known).
bool mvr_probe_layouts_to_check();
// Counts the input layouts the probe saw made (a layout looked up by its address before may be a new one since).
uint64_t mvr_probe_layout_generation();
// Counts the vertex shaders created on the hooked device (any thread): a shader looked up by its address before may be
// a new one since (mvr_pass.cpp's twin cache).
uint64_t mvr_shader_generation();
// Likewise for the pixel shaders (mvr_pass.cpp's mip bias twin cache).
uint64_t mvr_pixel_shader_generation();
// Where a vertex shader's bytecode reads its `worldMatrix` (the constant buffer slot and byte offset, from its
// reflection; kVsWorldMatrixGuid holds the same). False: it reads none.
bool mvr_world_matrix_place(const void* code, size_t size, unsigned& slot, unsigned& offset);

}  // namespace simqvfr::mvs
