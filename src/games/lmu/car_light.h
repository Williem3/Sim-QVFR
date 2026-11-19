// LMU's night car lighting ([quad] car_light, quad/car_lighting.h; docs/recon-lmu.md "Night car lighting"). At night LMU lights a car
// almost entirely with a few floodlights it hands each draw's vertex shader (GlobalCBFloats_OmniLights: position,
// radius, colour), evaluated per vertex with a linear falloff over 100-175 m, no ambient occlusion, no highlight and no
// shadow: the same light across the whole car, a toy's look. This pass relights the cars after the eye pass's scene,
// before the post-processing and DLSS read it: per pixel of every car part (the car slots' target, mvr_pass.h
// mvr_car_slots), the floodlights the part's draw was handed give a lacquer highlight, ambient occlusion from the
// depth darkens creases and contact, and a fill scale takes the flat light down. Nothing of the game's is changed: its
// scene image is added to and scaled by one draw (dual-source blending), the draws' own lights read as the game writes
// them (the buffer's Map handed our copy, mvr_pass_cars.h).
//
// The light table (CarLightTable) and the shading's math (car_light_*) are plain C++ for tests; CarLightPass is the
// D3D11 pass.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../quad/car_lighting.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;

namespace lmu {

using CarLightSettings = simqvfr::CarLighting;  // quad/car_lighting.h: [quad] car_light and its values

// One floodlight as LMU hands it to a draw: world position (metres), radius (its light reaches 0 there, linearly), the
// colour (linear, LMU's units: ~2 for a track floodlight).
struct CarLight {
    float pos[3] = {};
    float radius = 0;
    float colour[3] = {};
};

// The omni light buffer's layout (GlobalCBFloats_OmniLights in LMU's vertex shaders: omniPosRad[20], omniRGBDelta[20],
// float4 each), as the probe's reflection finds it (offsets in bytes).
struct OmniLayout {
    unsigned posOffset = 0, colourOffset = 320, count = 20;
};

// The frame's floodlights, each once (by position), and per car slot the ones its part's draws were handed that reach
// it (up to kPerSlot, nearest first). Cleared at each eye pass's start (the frame's lights are the same for both eyes;
// the slots' lists are rebuilt as the pass draws).
class CarLightTable {
public:
    static constexpr unsigned kMaxLights = 128, kPerSlot = 8, kNone = 0xff;
    explicit CarLightTable(unsigned slots = 4096) : slotLights_(slots), slotPaint_(slots, 0), slotFrame_(slots, 0) {}

    void begin_frame() {
        ++frame_;
        lights_.clear();
        firstSlot_ = ~0u;
        lastSlot_ = 0;
    }
    // A car part's draw (slot `slot`, its world matrix's translation `origin`) with the omni buffer `buffer` (its bytes
    // as the game wrote them, layout `layout`); `paint`: its pixel shader is a car body's (lacquer). `bufferGeneration`
    // changes with each write of the buffer: a part drawn again with the same buffer adds nothing new.
    void draw(unsigned slot, const float (&origin)[3], const uint8_t* buffer, size_t bufferBytes, const OmniLayout& layout,
              uint64_t bufferGeneration, bool paint);
    // The slots written this frame: [first, last] (first > last: none).
    unsigned first_slot() const { return firstSlot_; }
    unsigned last_slot() const { return lastSlot_; }
    // Slot `slot`'s lights packed 4 to a uint (8 bits each, kNone: none), and its flags (bit 0: paint), for the GPU's
    // per-slot buffer (uint4: lights 0-3, 4-7, flags, 0).
    void packed(unsigned slot, uint32_t (&out)[4]) const;
    const std::vector<CarLight>& lights() const { return lights_; }
    size_t slot_count() const { return slotLights_.size(); }

private:
    struct SlotLights {
        uint8_t index[kPerSlot];
        float distance[kPerSlot];
        uint8_t count = 0;
        uint64_t generation = ~0ull;
    };
    int light_index(const CarLight& l);  // its index in the frame's table (added if new), -1: full
    std::vector<CarLight> lights_;
    std::vector<SlotLights> slotLights_;
    std::vector<uint8_t> slotPaint_;
    std::vector<uint32_t> slotFrame_;
    uint32_t frame_ = 1;
    // The last buffer parsed (most draws repeat the draw before's lights: run 1 had ~1,000 car part draws a frame, and
    // each matched its 20 entries against the frame's table): its bytes, and per entry the light's index (-1 unusable).
    std::vector<uint8_t> lastBuffer_;
    int lastIndex_[32] = {};
    uint32_t lastFrame_ = 0;
    unsigned firstSlot_ = ~0u, lastSlot_ = 0;
};

// The pass's per-eye inputs.
struct CarLightView {
    unsigned eyeW = 0, eyeH = 0;            // the eye's region (top-left of the target)
    unsigned focusW = 0, focusH = 0;        // the focus view's region beside it (x from eyeW), as drawn; 0: none
    float remap[4] = {1, 0, 1, 0};          // the focus view's clip x = a x + b w, y = c y + d w (mvr_pass.h)
    float view[16] = {}, viewProj[16] = {}; // the eye's camera as its draws used it (row vectors, reversed depth)
};

// Pure math the shader does, for tests (the same formulas as kShader's).
// The view-space position of a pixel: `ndc` x, y in [-1, 1] (y up), `depth` as stored, `inverseProjection` row-major.
void car_light_unproject(const float (&inverseProjection)[16], float ndcX, float ndcY, float depth, float (&out)[3]);
// GGX highlight of one light at a point (all view space; n normalised): the radiance it adds.
float car_light_highlight(const float (&n)[3], const float (&toEye)[3], const float (&toLight)[3], float roughness);

// The D3D11 pass. Made on the render thread at its first run.
class CarLightPass {
public:
    CarLightPass();
    ~CarLightPass();
    CarLightPass(const CarLightPass&) = delete;
    CarLightPass& operator=(const CarLightPass&) = delete;

    // Relights `scene` (the eye pass's resolved scene, single-sample: an RGBA16F render target) where `slots` (the car
    // slots' target, R16_UINT, multisample or not) holds a car part, with `depth` (the pass's depth texture as it drew
    // it, R32, multisample alike). `table`'s lights and slots. The caller keeps the game's state (it is changed) and
    // the hooks out of the way. False: not drawn (not made, inputs missing, no car part this frame).
    bool run(ID3D11DeviceContext* context, ID3D11Resource* scene, ID3D11Resource* depth, ID3D11ShaderResourceView* slots,
             bool slotsMs, const CarLightTable& table, const CarLightView& view, const CarLightSettings& settings,
             int eye);
    // The lights' collection at the draws (the caller's measure, ms), counted into the CPU figure.
    void add_collect_ms(double ms);
    // Once a frame (after both eyes): the GPU timestamps of frames before read back where ready (never waits); every
    // 90 frames the averages below.
    void frame_end(ID3D11DeviceContext* context);
    // The last 90 frames' averages per frame (both eyes): GPU, and CPU (the pass's set-up and draw, and the lights'
    // collection at the draws). `windows`: how many such averages so far.
    double gpu_ms() const { return gpuMs_; }
    double cpu_ms() const { return cpuMs_; }
    unsigned long long windows() const { return windows_; }
    bool failed() const { return failed_; }

private:
    struct Impl;
    Impl* impl_;
    double gpuMs_ = 0, cpuMs_ = 0;
    unsigned long long windows_ = 0;
    bool failed_ = false;
};

}  // namespace lmu
