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
#include <string>
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
    explicit CarLightTable(unsigned slots = 4096)
        : slotLights_(slots), slotPaint_(slots, 0), slotFrame_(slots, 0), slotFrozen_(slots, 0) {}

    void begin_frame() {
        ++frame_;
        lights_.clear();
        spots_.clear();
        firstSlot_ = ~0u;
        lastSlot_ = 0;
        topFrame_ = 0;
    }
    // A car part's draw (slot `slot`, its world matrix's translation `origin`) with the omni buffer `buffer` (its bytes
    // as the game wrote them, layout `layout`); `paint`: its pixel shader is a car body's (lacquer); `normal`: the game's
    // normal for its pixels is in the normals' target (else the pass takes the depth's directions). `bufferGeneration`
    // changes with each write of the buffer: a part drawn again with the same buffer adds nothing new.
    void draw(unsigned slot, const float (&origin)[3], const uint8_t* buffer, size_t bufferBytes, const OmniLayout& layout,
              uint64_t bufferGeneration, bool paint, bool normal = false, bool ao = false, bool detail = false);
    // A car part's draw's spotlights (the headlights; `count` of the indices packed two 16-bit to a uint in `packed`,
    // each below `capacity`): kept for the frame, each once, up to kMaxSpots; and for slot `slot` (drawn this frame:
    // draw() first), up to kSpotsPerSlot of them, so its pixels weigh only the headlights LMU handed that part (the
    // frame's every one per pixel cost 0.35 ms GPU in the A/B of 2026-11-19).
    // `origin`, `spotBuffer`: the part's place and LMU's spotlights (64 bytes each, world space): a part handed more
    // than kSpotsPerSlot keeps the strongest at it (colour x attenuation x range falloff), not LMU's first.
    void add_spots(unsigned slot, uint32_t count, const uint32_t* packed, unsigned capacity, const float (&origin)[3],
                   const uint8_t* spotBuffer, size_t spotBytes);
    static constexpr unsigned kSpotsPerSlot = 6;
    // The frame's spotlights (LMU's indices) the slots packed so far use: packed() adds a slot's kept ones.
    const std::vector<uint16_t>& spots() const { return spots_; }
    static constexpr unsigned kMaxSpots = 32;
    // The frame's floodlights the most car parts were handed (the ground's shadows'), up to 4 (-1: none). Chosen at the
    // frame's first call (the first eye's pass) and kept for the frame: the second eye's draws add parts and could
    // reorder near ties, and each eye would shadow the ground from different lights.
    void top_lights(int (&out)[4]) const;
    // The slots written this frame: [first, last] (first > last: none).
    unsigned first_slot() const { return firstSlot_; }
    unsigned last_slot() const { return lastSlot_; }
    // Slot `slot`'s lights packed 4 to a uint (8 bits each, kNone: none), and its flags (bit 0: paint, bit 1: the game's
    // normal, bit 2: its baked occlusion, bit 3: its bent normal), for the GPU's per-slot buffer (uint4: lights 0-3,
    // 4-7, flags in bits 0-7 and spotlights 4-5 in bits 8-15 and 16-23, spotlights 0-3; the spotlights as indices into
    // spots(), kNone: none). A slot not drawn this frame: every list empty, no flags. A slot packed (a pass relit it)
    // keeps its lists for the frame: the other eye's pass draws it again and would otherwise light it differently.
    void packed(unsigned slot, uint32_t (&out)[4]) const;
    const std::vector<CarLight>& lights() const { return lights_; }
    size_t slot_count() const { return slotLights_.size(); }

private:
    struct SlotLights {
        uint8_t index[kPerSlot];
        float distance[kPerSlot];
        uint8_t count = 0;
        uint64_t generation = ~0ull;
        uint16_t spot[kSpotsPerSlot];  // LMU's indices
        float spotWeight[kSpotsPerSlot];
        uint8_t spotCount = 0;
    };
    int light_index(const CarLight& l);  // its index in the frame's table (added if new), -1: full
    std::vector<CarLight> lights_;
    std::vector<SlotLights> slotLights_;
    std::vector<uint8_t> slotPaint_;
    std::vector<uint32_t> slotFrame_;
    mutable std::vector<uint32_t> slotFrozen_;  // the frame a pass packed the slot (its lists kept from then on)
    mutable uint32_t topFrame_ = 0;              // top_lights' choice's frame
    mutable int top_[4] = {-1, -1, -1, -1};
    uint32_t frame_ = 1;
    // The last buffer parsed (most draws repeat the draw before's lights: run 1 had ~1,000 car part draws a frame, and
    // each matched its 20 entries against the frame's table): its bytes, and per entry the light's index (-1 unusable).
    mutable std::vector<uint16_t> spots_;  // the frame's spotlights the passes relit parts with (packed() adds)
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
    bool camera = true;                     // false: none found this pass (night vision's colour alone, if on)
};

// The comparison image (dev builds' Ctrl+Alt+L, qvfr_frame.cpp): `before` and `after` (BGRA, w x h each) side by side
// with a gap, a dark bar above with `beforeLabel` over the left and `afterLabel` over the right, as a PNG at `path`.
bool car_light_side_by_side(const std::wstring& path, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after,
                            unsigned w, unsigned h, const wchar_t* beforeLabel, const wchar_t* afterLabel);

// Pure math the shader does, for tests (the same formulas as kShader's).
// The view-space position of a pixel: `ndc` x, y in [-1, 1] (y up), `depth` as stored, `inverseProjection` row-major.
void car_light_unproject(const float (&inverseProjection)[16], float ndcX, float ndcY, float depth, float (&out)[3]);
// GGX highlight of one light at a point (all view space; n normalised): the radiance it adds.
float car_light_highlight(const float (&n)[3], const float (&toEye)[3], const float (&toLight)[3], float roughness);

// One block of frames' costs (the automatic A/B's): sums over the frames the pass ran in while the block was set.
struct CarLightBlock {
    unsigned serial = 0;  // the block's number (0: none)
    double gpuSum = 0;    // ms, over the timed runs
    unsigned gpuRuns = 0;
    unsigned runs = 0;    // runs drawn
    double cpuSum = 0;    // ms, set-up and draw plus the lights' collection
    unsigned frames = 0;
    // Per frame (both eyes): the timed runs' average times the runs per frame.
    double gpu_ms() const { return frames && gpuRuns ? gpuSum / gpuRuns * runs / frames : 0; }
    double cpu_ms() const { return frames ? cpuSum / frames : 0; }
};

// The automatic A/B (dev builds, Ctrl+Alt+N): blocks of kFrames frames in turn, the menu's settings ("all") and one
// variant: one switch that's on turned off, or every switch off (the relight's base). A, V1, A, V2, ... A, Vn, round
// after round, for up to kMaxFrames. Each variant block is paired with the A blocks either side of it (the scene
// changes slowly next to a block's length), so a switch's cost is the average of A - V over its pairs while driving.
// No D3D here: the caller sets the pass's block (frame()'s return) and hands back each ended block's sums (add()).
class CarLightAb {
public:
    static constexpr unsigned kFrames = 90;        // ~1 s a block at 90 Hz: long enough to see what changes
    static constexpr unsigned kMaxFrames = 27000;  // ~5 minutes at 90 Hz, then it stops by itself
    static constexpr int kMaxVariants = 7;

    // Starts (the variants from `light`'s switches now); false: nothing to vary (the relight or every switch off).
    bool start(const CarLightSettings& light);
    // Stops; report() keeps its figures.
    void stop() { active_ = false; }
    bool active() const { return active_; }
    // Once a frame before the passes: this frame's settings from the menu's (`light`, changed in a variant block).
    // Returns the frame's block serial for the pass; `ended`: a block serial the caller reads back and add()s now (0:
    // none; it ended a block's length ago, its GPU times read back since). Stops by itself after kMaxFrames.
    unsigned frame(CarLightSettings& light, unsigned& ended);
    // An ended block's sums (from the pass: CarLightPass::block).
    void add(const CarLightBlock& b);
    // A frame the A/B doesn't own (the comparison hotkey's), after this frame's frame(): the block it falls in (the
    // current one) isn't counted.
    void taint() {
        if (active_)
            tainted_[serial_ % kSeen] = true;
    }
    // One line a variant ("lacquer reflections: 0.412 ms GPU (+-0.030), 0.010 ms CPU (+-0.004), 18 pairs"), after
    // the menu settings' own cost.
    std::vector<std::string> report() const;
    unsigned rounds() const { return variants_ ? (serial_ - 1) / (2 * static_cast<unsigned>(variants_)) : 0; }
    unsigned frames() const { return frames_; }
    int variants() const { return variants_; }
    // The block now: -1 the menu's settings, else the variant.
    int kind() const { return kinds_[serial_ % kSeen]; }
    const char* variant_name(int v) const { return v >= 0 && v < variants_ ? names_[v] : "your settings"; }

private:
    struct Seen {
        unsigned serial = 0;
        int kind = -2;  // -1 A, >= 0 the variant
        double gpu = 0, cpu = 0;
        bool ok = false;  // counted: the pass ran and was timed, nothing tainted it
    };
    struct Sum {
        unsigned n = 0;
        double gpu = 0, gpu2 = 0, cpu = 0, cpu2 = 0;
        void add(double g, double c) {
            ++n;
            gpu += g;
            gpu2 += g * g;
            cpu += c;
            cpu2 += c * c;
        }
    };
    static constexpr unsigned kSeen = 8;
    void apply(int variant, CarLightSettings& light) const;
    bool active_ = false;
    int variants_ = 0;
    int switches_[kMaxVariants] = {};  // per variant: the switch turned off (0-5), or 6 every one
    const char* names_[kMaxVariants] = {};
    unsigned serial_ = 0, inBlock_ = 0, frames_ = 0;
    bool tainted_[kSeen] = {};
    int kinds_[kSeen] = {};
    Seen seen_[kSeen];
    Sum pairs_[kMaxVariants];
    Sum all_;
};

// The D3D11 pass. Made on the render thread at its first run.
class CarLightPass {
public:
    CarLightPass();
    ~CarLightPass();
    CarLightPass(const CarLightPass&) = delete;
    CarLightPass& operator=(const CarLightPass&) = delete;

    // Relights `scene` (the eye pass's resolved scene, single-sample: an RGBA16F render target) where `slots` (the car
    // slots' target, R16_UINT, multisample or not) holds a car part, with `depth` (the pass's depth texture as it drew
    // it, R32, multisample alike) and `normals` (the game's world normals, xyz * 0.5 + 0.5, multisample alike; null: the
    // depth's directions everywhere), `detail` (the normals the game's normal maps bent, alike; null: none). `table`'s
    // lights and slots. The caller keeps the game's state (it is changed) and
    // the hooks out of the way. False: not drawn (not made, inputs missing, no car part this frame).
    bool run(ID3D11DeviceContext* context, ID3D11Resource* scene, ID3D11Resource* depth, ID3D11ShaderResourceView* slots,
             bool slotsMs, const CarLightTable& table, const CarLightView& view, const CarLightSettings& settings,
             int eye, ID3D11ShaderResourceView* normals = nullptr, ID3D11ShaderResourceView* detail = nullptr,
             const uint8_t* spotBuffer = nullptr, size_t spotBytes = 0);
    // The lights' collection at the draws (the caller's measure, ms), counted into the CPU figure.
    void add_collect_ms(double ms);
    // Once a frame (after both eyes): the GPU timestamps of frames before read back where ready (never waits); every
    // 90 frames the averages below.
    void frame_end(ID3D11DeviceContext* context);
    // The last 90 frames' averages per frame (both eyes): GPU, and CPU (the pass's set-up and draw, and the lights'
    // collection at the draws). `windows`: how many such averages so far.
    double gpu_ms() const { return gpuMs_; }
    double cpu_ms() const { return cpuMs_; }
    // The automatic A/B's blocks: the runs and frames from now on count to block `serial` (0: none); its GPU times
    // count to it when they're read back, frames later. block(): a block of the last 32 (false: not one of them).
    void set_block(unsigned serial);
    bool block(unsigned serial, CarLightBlock& out) const;
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
