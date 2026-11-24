// The layer's process-wide switches that the games read and write (showcase.h, car_lighting.h, eye_off.h,
// probe/frame_parts.h): their one copy, here in the core. They were header statics before; a game built as its own DLL
// (mod_api.h) would have had a copy of its own and never seen the layer's.
#include <atomic>
#include <cstdint>

#include "../probe/frame_parts.h"
#include "car_lighting.h"
#include "eye_off.h"
#include "showcase.h"

namespace simqvfr {

namespace {
std::atomic<bool> stockWanted{false}, stockActive{false};
std::atomic<uint64_t> sceneFrames{0};
std::atomic<uint64_t> carLightingBits{car_lighting_bits(CarLighting{})};
std::atomic<int> eyeOff{kEyeOffNone};
}  // namespace

bool stock_wanted() { return stockWanted.load(std::memory_order_relaxed); }
void set_stock_wanted(bool on) { stockWanted.store(on, std::memory_order_relaxed); }
bool stock_active() { return stockActive.load(std::memory_order_relaxed); }
void set_stock_active(bool on) { stockActive.store(on, std::memory_order_relaxed); }
void note_scene_frame() { sceneFrames.fetch_add(1, std::memory_order_relaxed); }
uint64_t scene_frames() { return sceneFrames.load(std::memory_order_relaxed); }

uint64_t car_lighting_current_bits() { return carLightingBits.load(std::memory_order_relaxed); }
void set_car_lighting_bits(uint64_t bits) { carLightingBits.store(bits, std::memory_order_relaxed); }

int eye_off() { return eyeOff.load(std::memory_order_relaxed); }
void set_eye_off(int eye) { eyeOff.store(eye == 0 || eye == 1 ? eye : kEyeOffNone, std::memory_order_relaxed); }

FramePartSpan framePartSpans[static_cast<int>(FramePart::count)];

}  // namespace simqvfr
