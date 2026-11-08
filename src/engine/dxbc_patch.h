// DXBC (D3D11 shader model 4 and 5 bytecode) edits for drawing a second view in the same pass.
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

}  // namespace simqvfr::dxbc
