// VRAM census: how much of ACE's video memory follows the resolution it is launched at. The device's
// CreateCommittedResource* are patched (vtable entries, chained, so the D3D12 trace can patch them too)
// to record every 2D texture that can be a render target, depth target or UAV, at its allocation size.
// ACE makes its render targets as committed resources (d3d12 trace, 2026-11-02), so these are not
// aliased. A small tracker attached as the texture's private data takes it off again when the texture is
// destroyed (a new renderer after a graphics settings change frees the old one's).
// vram_census sorts the live textures by size: those that follow ACE's render or output target (the
// whole, a half, quarter or eighth on each axis) scale with the launch resolution; the rest (shadow maps,
// lookup tables, the desktop mirror) do not. The launcher scales the first part to estimate VRAM at other
// settings (launcher/settings.h's vram_estimate).
#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D12Device;

namespace simqvfr {

bool vram_census_start(ID3D12Device* device);

struct VramCensus {
    uint64_t scaledBytes = 0;  // textures that follow the render or output target
    uint64_t otherBytes = 0;   // the other recorded textures
    size_t scaledTextures = 0, otherTextures = 0;
};
// The live textures against the render and output target sizes (0: unknown, then everything is "other").
VramCensus vram_census(int renderWidth, int renderHeight, int outputWidth, int outputHeight);

// Whether a texture of this size follows a target of that size: each axis the target's divided by 1, 2,
// 4 or 8 (rounded either way).
bool vram_follows(uint64_t width, uint32_t height, int targetWidth, int targetHeight);

}  // namespace simqvfr
