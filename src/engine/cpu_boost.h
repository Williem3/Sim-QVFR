// What Sim-QVFR sets on the game's own process for its CPU time ([cpu], live; the layer calls it inside the game, so it
// needs no handle to a protected process). full_speed=1 (the default): EcoQoS off, so Windows never runs the game in
// efficiency mode, as it may for a process whose window is behind others (a VR game's desktop window usually is).
// full_speed=0: Windows decides again. Only the EcoQoS bit is touched: the process's other power-throttling choices
// (LMU keeps its timer requests when hidden) stay as the game set them. probe/cpu_placement.h logs the state.
#pragma once

#include <cstdint>

namespace simqvfr {

// A process's power throttling as PROCESS_POWER_THROTTLING_STATE has it: the policies it controls and which are on.
struct PowerThrottling {
    uint32_t control = 0, state = 0;
    bool operator==(const PowerThrottling&) const = default;
};
inline constexpr uint32_t kThrottleExecutionSpeed = 0x1;  // PROCESS_POWER_THROTTLING_EXECUTION_SPEED: EcoQoS

// `now` with EcoQoS off (`full`: controlled, not on) or back to Windows (not controlled); the other bits kept.
PowerThrottling with_full_speed(PowerThrottling now, bool full);

// The calling process's power throttling; false when Windows can't say.
bool read_power_throttling(PowerThrottling& out);
// Sets the calling process's EcoQoS as with_full_speed. False when Windows refuses (the state is then unchanged).
bool cpu_set_full_speed(bool full);

}  // namespace simqvfr
