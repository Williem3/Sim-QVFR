#include "views3.h"

#include <cstdint>
#include <cstring>
#include <iterator>

#include "abi.h"
#include "../../log.h"
#include "../../probe/probe.h"
#include "../../xr.h"

namespace aceqv {
namespace {

constexpr uint64_t kReportEvery = 900;
bool patched = false;
uint64_t beginCalls = 0;  // renderer begin runs on the main thread only

struct RendererState {
    int32_t configured, active, frameIndex;
    long long vectorCounts[std::size(ace::kRendererPerViewVectors)];
};

// No C++ objects with destructors here: __try needs a plain frame.
bool read_state(const uint8_t* renderer, RendererState* out) {
    __try {
        out->configured = *reinterpret_cast<const int32_t*>(renderer + ace::kRendererConfiguredViewMode);
        out->active = *reinterpret_cast<const int32_t*>(renderer + ace::kRendererActiveViewMode);
        out->frameIndex = *reinterpret_cast<const int32_t*>(renderer + ace::kRendererFrameIndex);
        for (size_t i = 0; i < std::size(ace::kRendererPerViewVectors); ++i) {
            const auto& v = ace::kRendererPerViewVectors[i];
            const auto* const* bounds = reinterpret_cast<const uint8_t* const*>(renderer + v.offset);  // first, last
            out->vectorCounts[i] = static_cast<long long>((bounds[1] - bounds[0]) / static_cast<ptrdiff_t>(v.elementSize));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void on_renderer_begin(void* renderer) {
    const uint64_t call = ++beginCalls;
    if (call > 3 && call % kReportEvery != 0)
        return;
    RendererState s{};
    if (!read_state(static_cast<const uint8_t*>(renderer), &s)) {
        logf("views3: renderer=%p unreadable", renderer);
        return;
    }
    // Renderer begin sets the active mode after this entry hook, so the configured mode is the one
    // that matters; the active one is still last frame's.
    logf("views3: renderer begin call=%llu renderer=%p configured_mode=%d active_mode=%d frame_index=%d "
         "per-view vectors=%lld,%lld (%s)",
         static_cast<unsigned long long>(call), renderer, s.configured, s.active, s.frameIndex, s.vectorCounts[0],
         s.vectorCounts[1],
         // The vectors are filled only by the triple-screen setup; in VR they stay empty (views3 run 1).
         s.configured == 2 ? "3 views, patch took effect"
         : s.configured == 1 ? "2 views: renderer was created before the patch"
                             : "unexpected");
}

}  // namespace

bool views3_patch() {
    if (patched)
        return true;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* immediate = reinterpret_cast<uint8_t*>(base + ace::kInitVrModeImmediateRva);
    if (std::memcmp(immediate - 1, ace::kInitVrModeAnchor, sizeof(ace::kInitVrModeAnchor)) != 0) {
        log_line("views3: renderer init anchor mismatch; experiment refused");
        return false;
    }
    DWORD protect = 0;
    if (!VirtualProtect(immediate, 1, PAGE_EXECUTE_READWRITE, &protect)) {
        log_line("views3: VirtualProtect failed; experiment refused");
        return false;
    }
    *immediate = 2;
    VirtualProtect(immediate, 1, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), immediate, 1);
    patched = true;
    log_line("views3: renderer init patched, VR selects view mode 2 (3 views)");
    return true;
}

void views3_watch() {
    if (!probe_set_observer("renderer_begin_1d6d7f0", &on_renderer_begin))
        log_line("views3: renderer begin probe not found; no view-mode report");
}

}  // namespace aceqv
