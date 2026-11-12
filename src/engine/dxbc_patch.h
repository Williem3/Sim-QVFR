// DXBC (D3D11 shader model 4 and 5 bytecode) edits for drawing a second view in the same pass: for NVIDIA's multi-view
// rendering (add_second_view) or for any GPU by instancing (add_instanced_view, below).
//
// add_second_view turns a vertex shader into one that also writes the second view's clip position, for NVIDIA's
// single-pass stereo with a full XYZW second position (NV_XYZW_RIGHT): second = (a x + b w, c y + d w, z, w) of its
// SV_Position, with (a, b, c, d) the x and y of a constant buffer's first vector (`cbSlot`, bound by the caller). That
// is quad/focus clip remap: a focus view that shares its eye's camera and differs only in the projection's x and y
// scale and offset. It also writes the viewport mask (NV_VIEWPORT_MASK): with NVAPI's independent viewport mask on,
// its low 16 bits are view 0's viewports and its high 16 bits view 1's (tests/nvapi_stereo_check.cpp, RTX 5070 Ti,
// 2026-11-08; NVAPI's second-mask semantic type isn't read for two views). The default sends view 0 to viewport 0 and
// view 1 to viewport 1 of the same target.
//
// How: every instruction that writes SV_Position is cloned to write a new temp as well (shader model 4 and 5 can't
// read an output back); before each ret the second position and the mask are written from it. The output signature
// gains the new elements after the last register, so a pixel shader linked to the original still links. The
// container's checksum is recomputed (the modified MD5 the runtime checks).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace simqvfr::dxbc {

// The checksum DXBC containers carry at bytes 4-19: MD5 over bytes 20 to the end, with DXBC's own final block.
void checksum(const uint8_t* data, size_t size, uint32_t (&out)[4]);
bool checksum_ok(const uint8_t* data, size_t size);
void sign(std::vector<uint8_t>& container);  // writes the checksum

inline constexpr const char* kRightSemantic = "NV_XYZW_RIGHT";
inline constexpr const char* kMaskSemantic = "NV_VIEWPORT_MASK";

struct SecondViewOptions {
    uint32_t cbSlot = 13;          // the remap's constant buffer: its first vector is (a, b, c, d)
    bool viewportMask = true;      // write NV_VIEWPORT_MASK
    uint32_t mask = 0x00020001;    // its value: view 0's viewports in bits 0-15, view 1's in bits 16-31
};

struct SecondView {
    std::vector<uint8_t> bytecode;  // empty when not converted (`error` says why)
    std::string error;
    uint32_t rightRegister = 0;     // NV_XYZW_RIGHT's output register (.xyzw)
    bool hasMask = false;
    uint32_t maskRegister = 0;      // NV_VIEWPORT_MASK's output register (.x)
    uint32_t positionRegister = 0;  // the shader's own SV_Position register
    int positionWrites = 0;         // instructions that write it (each cloned)
};

SecondView add_second_view(const uint8_t* data, size_t size, const SecondViewOptions& options = {});

// add_instanced_view is the vendor-neutral way to the same two views (any D3D11.3 GPU that lets a vertex shader pick
// the viewport: D3D11_FEATURE_DATA_D3D11_OPTIONS3::VPAndRTArrayIndexFromAnyShaderFeedingRasterizer). The caller draws
// every draw with twice its instances; the converted shader takes view = SV_InstanceID & 1 and, for any use of the
// instance it had, the game's own instance SV_InstanceID >> 1 (per-instance vertex data needs its step rate doubled in
// the input layout, which is the caller's). View 0 writes SV_Position exactly as before, view 1 the clip remap of it
// (cbSlot's first vector, as add_second_view); SV_ViewportArrayIndex = view, so view 0 lands in viewport 0 and view 1
// in viewport 1.
//
// How: every instruction that wrote SV_Position writes a new temp instead; before each ret SV_Position is that temp or
// its remap, chosen by the view (movc), and the viewport index is written. SV_InstanceID joins the input signature after
// its last register if the shader didn't read it (a system value: the input layout needn't change); a shader that did
// reads the halved one from a temp. SV_ViewportArrayIndex joins the output signature after its last register (a pixel
// shader linked to the original still links). The SFI0 chunk gains the feature's flag, as the compiler sets it.
inline constexpr const char* kViewportIndexSemantic = "SV_ViewportArrayIndex";
inline constexpr const char* kInstanceIdSemantic = "SV_InstanceID";
inline constexpr uint64_t kRequiresViewportIndexFromAnyShader = 0x2000;  // SFI0: ..._FROM_ANY_SHADER_FEEDING_RASTERIZER

struct InstancedViewOptions {
    uint32_t cbSlot = 13;  // the remap's constant buffer: its first vector is (a, b, c, d)
};

struct InstancedView {
    std::vector<uint8_t> bytecode;  // empty when not converted (`error` says why)
    std::string error;
    uint32_t positionRegister = 0;  // the shader's SV_Position output register
    int positionWrites = 0;         // instructions that wrote it (each redirected)
    uint32_t viewportRegister = 0;  // SV_ViewportArrayIndex's output register (.x)
    uint32_t instanceRegister = 0;  // SV_InstanceID's input register (.x)
    bool hadInstance = false;       // the shader read SV_InstanceID itself
    int instanceReads = 0;          // its instructions reading it (now the halved one)
};

InstancedView add_instanced_view(const uint8_t* data, size_t size, const InstancedViewOptions& options = {});

// add_focus_mip_bias is for DLSS super resolution on a focus view drawn in its eye's pass (either way above: view 1 in
// its own viewport of the same target). DLSS wants textures sampled with a negative mip bias where it upscales, but one
// draw draws both views through the same sampler, so the bias comes from the pixel shader: its plain samples (sample,
// with or without offsets) become sample_b with a bias of the constant buffer's second vector's x inside the rectangle
// of its first vector (x0, y0, x1, y1 in target pixels: the focus view's viewport) and its y outside it (the eye view's
// own; 0 samples as before). NVIDIA's DLSS guide (3.5): bias = log2(render width / output width) - 1, so -1 under DLAA.
// Depth compares (shadows), explicit LODs, gradients, gathers and the shader's own sample_b stay as they are.
//
// Needs SV_Position in the input signature (LMU's scene pixel shaders have it, mostly without reading it: the
// declaration is added then). How: before the first instruction the bias goes into a new temp (ge and lt against the
// rectangle, and-ed together, then a movc of the two biases); each sample's opcode becomes sample_b with that temp's x
// as its last operand. Checked on a WARP device and on LMU's 133 scene pixel shaders (tests/dxbc_patch_test.cpp).
struct FocusMipBiasOptions {
    uint32_t cbSlot = 13;  // (x0, y0, x1, y1), then (focus view's bias, eye view's bias, -, -)
};

struct FocusMipBias {
    std::vector<uint8_t> bytecode;  // empty when not converted (`error` says why)
    std::string error;
    uint32_t positionRegister = 0;  // SV_Position's input register
    bool positionRead = false;      // the shader declared it itself
    int samples = 0;                // sample instructions now biased
    int leftAlone = 0;              // other sample kinds left as they were
};

FocusMipBias add_focus_mip_bias(const uint8_t* data, size_t size, const FocusMipBiasOptions& options = {});

// add_slot_output is for other cars' motion vectors (docs/recon-lmu.md "stage 3"): a pixel shader that also writes, to
// render target `reg` (SV_Target1 by default, a uint), the first element of a uint buffer at `srvSlot`: the draw's car
// part slot, bound per draw by the caller (unbound: 0, none). Everything the shader wrote it still writes.
//
// How: dcl_resource_buffer (uint) t<srvSlot> and dcl_output o<reg>.x join the declarations, and before the first
// instruction ld o<reg>.x, l(0, 0, 0, 0), t<srvSlot>.xxxx (an output written once and never again: SM4/5 keep it). The
// output signature gains SV_Target<reg> (uint, x) after its last element. Refused when the shader already has that
// output or declares that resource, or isn't a pixel shader.
struct SlotOutputOptions {
    uint32_t srvSlot = 127;
    uint32_t reg = 1;
};

struct SlotOutput {
    std::vector<uint8_t> bytecode;  // empty when not converted (`error` says why)
    std::string error;
};

SlotOutput add_slot_output(const uint8_t* data, size_t size, const SlotOutputOptions& options = {});

}  // namespace simqvfr::dxbc
