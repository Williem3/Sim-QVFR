#include "cpu_boost.h"

#include <windows.h>

namespace simqvfr {

PowerThrottling with_full_speed(PowerThrottling now, bool full) {
    PowerThrottling out = now;
    out.state &= ~kThrottleExecutionSpeed;  // never on
    if (full)
        out.control |= kThrottleExecutionSpeed;
    else
        out.control &= ~kThrottleExecutionSpeed;
    return out;
}

bool read_power_throttling(PowerThrottling& out) {
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (!GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)))
        return false;
    out = {s.ControlMask, s.StateMask};
    return true;
}

bool cpu_set_full_speed(bool full) {
    PowerThrottling now;
    if (!read_power_throttling(now))
        now = {};  // nothing known: only the EcoQoS bit
    const PowerThrottling want = with_full_speed(now, full);
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = want.control;
    s.StateMask = want.state;
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)) != FALSE;
}

}  // namespace simqvfr
