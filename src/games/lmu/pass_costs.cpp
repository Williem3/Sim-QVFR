// What LMU's QVFR passes cost (CPU clocks, GPU spans) and the A/B runs' sides: qvfr_frame_internal.h.
#include "qvfr_frame_internal.h"

#include <windows.h>
#include <intrin.h>

namespace lmu::qvfr_frame_detail {

// ---- QVFR: what the four passes cost ----
// CPU: each pass's time on the render thread, wall clock and the thread's own CPU (its cycle counter), and this code's
// own work in its hooks. GPU: timestamp queries around this code's copies, read back frames later without stalling.
// Averages logged every 900 frames while the scene runs (qvfr_frame_frame_end).
double qpcPerMs = 0, tscPerMs = 0;
long long calibQpc = 0;
unsigned long long calibTsc = 0;

double now_ms() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (qpcPerMs == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcPerMs = static_cast<double>(f.QuadPart) / 1000.0;
    }
    return static_cast<double>(t.QuadPart) / qpcPerMs;
}

unsigned long long thread_cycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    // The cycle counter's rate (TSC): calibrated against QPC over the first second.
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (calibQpc == 0) {
        calibQpc = t.QuadPart;
        calibTsc = __rdtsc();
    } else if (tscPerMs == 0 && qpcPerMs > 0 && static_cast<double>(t.QuadPart - calibQpc) > 1000 * qpcPerMs) {
        tscPerMs = static_cast<double>(__rdtsc() - calibTsc) / (static_cast<double>(t.QuadPart - calibQpc) / qpcPerMs);
    }
    return c;
}

CostWindow costWindow;
GpuSpans gpuSpans;

// lmu_mvr_backend_ab's side of this frame: 0 NVIDIA's single-pass stereo, 1 instancing.
int backendSide = 0;
bool backendAbOk = false, backendAbLogged = false;
// lmu_car_vectors_ab's: 0 with the other cars' motion, 1 without; the cars' motion went on at the install.
int carSide = 0;
bool carAbLogged = false, carsOn = false;
std::atomic<bool> carsWanted{true};  // qvfr_frame_cars_wanted
bool carsPaused = false;
bool carLightLogged = false, carLightSaid = false;  // night car lighting's switch as last logged             // the tracking paused by it (logged when it changes)
bool car_ab_on() { return cfg.carAb > 0 && carsOn; }
// lmu_mask_ab's: 0 with the lens mask's and the focus mask's draws, 1 without.
int maskSide = 0;
bool maskAbLogged = false;

// The A/B side of this frame: lmu_mask_ab's (0 with the masks, 1 without), lmu_mvr_backend_ab's (0 NVIDIA's, 1
// instancing) or lmu_car_vectors_ab's (0 with the cars' motion, 1 without).
int ab_side() { return cfg.maskAb > 0 ? maskSide : cfg.backendAb > 0 ? backendSide : carSide; }
bool ab_run() { return cfg.maskAb > 0 || (cfg.backendAb > 0 && backendAbOk) || car_ab_on(); }

AbWindow ab;

}  // namespace lmu::qvfr_frame_detail
