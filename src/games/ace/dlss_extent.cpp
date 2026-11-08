#include "dlss_extent.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

#include <MinHook.h>

#include "abi.h"
#include "ngx_extent.h"
#include "../../engine/code_patch.h"
#include "../../log.h"

namespace simqvfr {
namespace {

constexpr uint64_t kReportEveryFrames = 900;

// NVSDK_NGX_D3D12_CreateFeature(command list, feature, parameters, NVSDK_NGX_Handle** out) and
// NVSDK_NGX_D3D12_EvaluateFeature_C(command list, const NVSDK_NGX_Handle*, parameters, progress callback).
using CreateFn = unsigned (*)(void* commandList, unsigned feature, void* bag, void** outHandle);
using EvaluateFn = unsigned (*)(void* commandList, const void* handle, void* bag, void* callback);
CreateFn createOriginal = nullptr;
EvaluateFn evaluateOriginal = nullptr;
bool installed = false;

SRWLOCK lock = SRWLOCK_INIT;
NgxExtents extents;
// Over the report window: evaluations of a recorded feature, those whose bag held another size, and evaluations of a
// handle never seen made (made before the hooks, or not SuperSampling).
std::atomic<uint64_t> evaluated{0}, corrected{0}, unknown{0};
uint64_t frames = 0;
bool reported = false;  // a window with evaluations was logged (an idle one then is too, once)

unsigned createHook(void* commandList, unsigned feature, void* bag, void** outHandle) {
    // What ACE has just put in the bag for this feature (read before NGX or a layer below it sees the call).
    unsigned w = 0, h = 0, rw = 0, rh = 0;
    const bool dlss = feature == kNgxFeatureSuperSampling && bag && ngx_get_ui(bag, "OutWidth", w) &&
                      ngx_get_ui(bag, "OutHeight", h);
    if (dlss) {
        ngx_get_ui(bag, "Width", rw);
        ngx_get_ui(bag, "Height", rh);
    }
    const unsigned result = createOriginal(commandList, feature, bag, outHandle);
    if (dlss && ngx_succeeded(result) && outHandle && *outHandle && w > 0 && h > 0) {
        AcquireSRWLockExclusive(&lock);
        extents.record(*outHandle, w, h);
        ReleaseSRWLockExclusive(&lock);
        logf("views4 dlss: feature 0x%llx made with output %ux%u (render up to %ux%u)",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(*outHandle)), w, h, rw, rh);
    }
    return result;
}

unsigned evaluateHook(void* commandList, const void* handle, void* bag, void* callback) {
    if (handle && bag) {
        unsigned w = 0, h = 0;
        AcquireSRWLockShared(&lock);
        const bool known = extents.find(handle, w, h);
        ReleaseSRWLockShared(&lock);
        if (known) {
            evaluated.fetch_add(1, std::memory_order_relaxed);
            if (ngx_put_output_extent(bag, w, h))
                corrected.fetch_add(1, std::memory_order_relaxed);
        } else {
            unknown.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return evaluateOriginal(commandList, handle, bag, callback);
}

}  // namespace

bool dlss_extent_start() {
    if (installed)
        return true;
    if (!code_matches(ace::kNgxCreateFeatureRva, ace::kNgxCreateFeatureAnchor, sizeof(ace::kNgxCreateFeatureAnchor)) ||
        !code_matches(ace::kNgxEvaluateFeatureRva, ace::kNgxEvaluateFeatureAnchor,
                      sizeof(ace::kNgxEvaluateFeatureAnchor))) {
        log_line("views4 dlss: NGX create/evaluate anchor mismatch; the DLSS output sizes are left to ACE");
        return false;
    }
    void* create = reinterpret_cast<void*>(module_base() + ace::kNgxCreateFeatureRva);
    void* evaluate = reinterpret_cast<void*>(module_base() + ace::kNgxEvaluateFeatureRva);
    MH_STATUS s = MH_CreateHook(create, reinterpret_cast<void*>(&createHook), reinterpret_cast<void**>(&createOriginal));
    if (s == MH_OK) {
        s = MH_CreateHook(evaluate, reinterpret_cast<void*>(&evaluateHook), reinterpret_cast<void**>(&evaluateOriginal));
        if (s != MH_OK)
            MH_RemoveHook(create);
    }
    if (s == MH_OK && (s = MH_QueueEnableHook(create)) == MH_OK && (s = MH_QueueEnableHook(evaluate)) == MH_OK)
        s = MH_ApplyQueued();
    if (s != MH_OK) {
        MH_RemoveHook(create);
        MH_RemoveHook(evaluate);
        logf("views4 dlss: NGX create/evaluate hooks failed (MinHook %s); the DLSS output sizes are left to ACE",
             MH_StatusToString(s));
        return false;
    }
    installed = true;
    log_line("views4 dlss: each DLSS feature's output size goes into NGX's parameters at its evaluation "
             "(views4_dlss_extent)");
    return true;
}

void dlss_extent_frame_end() {
    if (!installed || ++frames % kReportEveryFrames != 0)
        return;
    const uint64_t n = evaluated.exchange(0), fixed = corrected.exchange(0), other = unknown.exchange(0);
    if (n == 0 && other == 0 && !reported)
        return;  // no DLSS (yet)
    reported = n > 0 || other > 0;
    logf("views4 dlss: last %llu frames: %llu evaluations, %llu with another feature's output size in the "
         "parameters (set to their own), %llu of features not seen made",
         static_cast<unsigned long long>(kReportEveryFrames), static_cast<unsigned long long>(n),
         static_cast<unsigned long long>(fixed), static_cast<unsigned long long>(other));
}

}  // namespace simqvfr
