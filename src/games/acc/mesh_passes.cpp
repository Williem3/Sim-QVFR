#include "mesh_passes.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "abi.h"

namespace acc {
namespace {

using SetupFn = void (*)(void* pass, void* scene, void* view, int passType, int depthStencil, void* processor,
                         void* dynamicElements, void* dynamicRelevance, int numDynamic, void* buildRequests,
                         int numBuildRequests, void* meshDrawCommands, void* mobileProcessor, void* mobileCommands);
using DrawFn = void (*)(void* pass, void* parallelSet, void* rhiCmdList);
using RenderFamilyFn = void (*)(void* rhiCmdList, void* renderer);
// InitViews returns a bool in UE 4.26; rax is passed through whole.
using InitViewsFn = std::uint64_t (*)(void* renderer, void* rhiCmdList, int depthStencil, void* ilcTaskData,
                                      void* updateEvents);

SetupFn originalSetup = nullptr;
DrawFn originalDraw = nullptr;
RenderFamilyFn originalRenderFamily = nullptr;
InitViewsFn originalInitViews = nullptr;

// The visibility steps (abi.h [Visibility]): pass-through detours with at least as many integer/pointer arguments as
// each function takes (none takes a float); extra ones read the caller's stack and go back unused. rax passes through.
using U = std::uint64_t;
using Fn4 = U (*)(U, U, U, U);
using Fn8 = U (*)(U, U, U, U, U, U, U, U);
using Fn10 = U (*)(U, U, U, U, U, U, U, U, U, U);
Fn8 originalComputeVisibility = nullptr;
Fn4 originalPrecomputed = nullptr, originalCullStep = nullptr, originalOcclusion = nullptr, originalSetupMeshPass = nullptr;
Fn8 originalRelevance = nullptr;
Fn10 originalGather = nullptr;

PassProbeConfig config;
std::atomic<bool> installed{false};

constexpr int kStereoPasses = 5, kTypes = 32, kStates = 2;
constexpr int kFocusPrimary = 3;  // eSSP_LEFT_EYE_SIDE: the focus pair's primary view (abi.h kViewStereoPass)

struct PassStat {
    std::uint64_t setups = 0, draws = 0, dispatches = 0, parallel = 0, skipped = 0;
    std::int64_t dispatchTicks = 0;
};
// Visibility steps per view: the precomputed-visibility lookup, the culling block after it (frustum culling and the
// other parallel loops, up to the next step), that step, occlusion culling, relevance, mesh pass setup.
enum VisStage { kPrecomputed, kCullBlock, kCullStep, kOcclusion, kRelevance, kMeshSetup, kVisStages };
constexpr const char* kVisStageNames[kVisStages] = {"precomputed", "culling", "cull-step", "occlusion", "relevance",
                                                    "mesh-setup"};
struct StateStat {
    std::uint64_t frames = 0;
    std::int64_t renderTicks = 0, initViewsTicks = 0, visibilityTicks = 0, gatherTicks = 0;
    std::int64_t visTicks[kStereoPasses][kVisStages] = {};
    PassStat pass[kStereoPasses][kTypes];
};
// The render thread's (every hook runs there) and the report's, under `mutex`.
std::mutex mutex;
StateStat stats[kStates];
int renderState = 0;                 // the main family's frame being rendered: 0 all drawn, 1 focus skipped
std::uint64_t familyFrames = 0;      // main-family scene renders
bool inMainFamily = false;
bool firstFrameLogged = false;
std::string firstFrame;              // one frame's setups, logged once
std::uint64_t reportFrames = 0;
std::int64_t qpcFrequency = 0;

std::int64_t now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int stereo_of(const void* view) {
    if (!view)
        return -1;
    const int s = *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(view) + kViewStereoPass);
    return s >= 0 && s < kStereoPasses ? s : -1;
}

void setup_hook(void* pass, void* scene, void* view, int passType, int depthStencil, void* processor,
                void* dynamicElements, void* dynamicRelevance, int numDynamic, void* buildRequests, int numBuildRequests,
                void* meshDrawCommands, void* mobileProcessor, void* mobileCommands) {
    const int stereo = stereo_of(view);
    bool skip = false;
    {
        std::lock_guard guard(mutex);
        if (inMainFamily && stereo >= 0 && passType >= 0 && passType < kTypes) {
            PassStat& s = stats[renderState].pass[stereo][passType];
            ++s.setups;
            skip = renderState == 1 && config.skipAbFrames > 0 && stereo == kFocusPrimary && !translucency_pass(passType);
            if (skip)
                ++s.skipped;
        }
    }
    if (skip) {
        // Nothing to build: no dynamic elements, no build requests, no visible cached commands. MaxNumDraws becomes 0,
        // so no setup task starts and DispatchDraw returns at once. The visible commands' array is the frame's memstack
        // (no destructors); DispatchPassSetup swaps it into the pass as it is.
        numDynamic = 0;
        numBuildRequests = 0;
        if (meshDrawCommands)
            *reinterpret_cast<int*>(static_cast<std::uint8_t*>(meshDrawCommands) + kArrayNum) = 0;
    }
    originalSetup(pass, scene, view, passType, depthStencil, processor, dynamicElements, dynamicRelevance, numDynamic,
                  buildRequests, numBuildRequests, meshDrawCommands, mobileProcessor, mobileCommands);
    if (!inMainFamily || stereo < 0 || passType < 0 || passType >= kTypes)
        return;
    const int draws = *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(pass) + kPassMaxNumDraws);
    std::lock_guard guard(mutex);
    stats[renderState].pass[stereo][passType].draws += draws > 0 ? static_cast<std::uint64_t>(draws) : 0;
    if (!firstFrameLogged && familyFrames == 2) {
        char item[48];
        std::snprintf(item, sizeof(item), " %d/%d:%d", stereo, passType, draws);
        firstFrame += item;
    }
}

void draw_hook(void* pass, void* parallelSet, void* rhiCmdList) {
    const std::int64_t t0 = now();
    originalDraw(pass, parallelSet, rhiCmdList);
    const std::int64_t t1 = now();
    const auto* p = static_cast<const std::uint8_t*>(pass);
    const int stereo = stereo_of(*reinterpret_cast<void* const*>(p + kPassView));
    const int type = *reinterpret_cast<const int*>(p + kPassType);
    if (stereo < 0 || type < 0 || type >= kTypes)
        return;
    std::lock_guard guard(mutex);
    if (!inMainFamily)
        return;
    PassStat& s = stats[renderState].pass[stereo][type];
    ++s.dispatches;
    s.parallel += parallelSet != nullptr;
    s.dispatchTicks += t1 - t0;
}

std::uint64_t init_views_hook(void* renderer, void* rhiCmdList, int depthStencil, void* ilcTaskData, void* updateEvents) {
    const std::int64_t t0 = now();
    const std::uint64_t r = originalInitViews(renderer, rhiCmdList, depthStencil, ilcTaskData, updateEvents);
    const std::int64_t t1 = now();
    std::lock_guard guard(mutex);
    if (inMainFamily)
        stats[renderState].initViewsTicks += t1 - t0;
    return r;
}

// ---- Visibility (render thread only, inside the main family's InitViews) ----

std::int64_t precomputedExit[kStereoPasses] = {};  // when view s's precomputed-visibility lookup returned (0: not yet)

void add_vis(int stereo, VisStage stage, std::int64_t ticks) {
    if (stereo < 0)
        return;
    std::lock_guard guard(mutex);
    if (inMainFamily)
        stats[renderState].visTicks[stereo][stage] += ticks;
}

U compute_visibility_hook(U a, U b, U c, U d, U e, U f, U g, U h) {
    for (std::int64_t& t : precomputedExit)
        t = 0;
    const std::int64_t t0 = now();
    const U r = originalComputeVisibility(a, b, c, d, e, f, g, h);
    const std::int64_t t1 = now();
    std::lock_guard guard(mutex);
    if (inMainFamily)
        stats[renderState].visibilityTicks += t1 - t0;
    return r;
}

U precomputed_hook(U viewState, U view, U scene, U d) {
    const std::int64_t t0 = now();
    const U r = originalPrecomputed(viewState, view, scene, d);
    const std::int64_t t1 = now();
    const int stereo = stereo_of(reinterpret_cast<void*>(view));
    add_vis(stereo, kPrecomputed, t1 - t0);
    if (stereo >= 0)
        precomputedExit[stereo] = t1;
    return r;
}

U cull_step_hook(U scene, U view, U c, U d) {
    const std::int64_t t0 = now();
    const int stereo = stereo_of(reinterpret_cast<void*>(view));
    if (stereo >= 0 && precomputedExit[stereo]) {
        add_vis(stereo, kCullBlock, t0 - precomputedExit[stereo]);
        precomputedExit[stereo] = 0;
    }
    const U r = originalCullStep(scene, view, c, d);
    add_vis(stereo, kCullStep, now() - t0);
    return r;
}

U occlusion_hook(U cmdList, U scene, U view, U dynamicVertexBuffer) {
    const std::int64_t t0 = now();
    const U r = originalOcclusion(cmdList, scene, view, dynamicVertexBuffer);
    add_vis(stereo_of(reinterpret_cast<void*>(view)), kOcclusion, now() - t0);
    return r;
}

U relevance_hook(U cmdList, U scene, U view, U viewCommands, U viewBit, U f, U g, U h) {
    const std::int64_t t0 = now();
    const U r = originalRelevance(cmdList, scene, view, viewCommands, viewBit, f, g, h);
    add_vis(stereo_of(reinterpret_cast<void*>(view)), kRelevance, now() - t0);
    return r;
}

U setup_mesh_pass_hook(U renderer, U view, U depthStencil, U viewCommands) {
    const std::int64_t t0 = now();
    const U r = originalSetupMeshPass(renderer, view, depthStencil, viewCommands);
    add_vis(stereo_of(reinterpret_cast<void*>(view)), kMeshSetup, now() - t0);
    return r;
}

// h: HasDynamicMeshElementsMasks (TArray<uint8>: data, Num at +8), one view mask per primitive.
U gather_hook(U a, U b, U c, U d, U e, U f, U g, U h, U i, U j) {
    bool clear = false;
    {
        std::lock_guard guard(mutex);
        clear = inMainFamily && renderState == 1 && config.gather_ab();
    }
    if (clear && h) {
        auto* masks = *reinterpret_cast<std::uint8_t**>(h);
        const int count = *reinterpret_cast<const int*>(reinterpret_cast<const std::uint8_t*>(h) + kArrayNum);
        for (int k = 0; masks && k < count; ++k)
            masks[k] &= static_cast<std::uint8_t>(~kFocusViewBits);
    }
    const std::int64_t t0 = now();
    const U r = originalGather(a, b, c, d, e, f, g, h, i, j);
    const std::int64_t t1 = now();
    std::lock_guard guard(mutex);
    if (inMainFamily)
        stats[renderState].gatherTicks += t1 - t0;
    return r;
}

void render_family_hook(void* rhiCmdList, void* renderer) {
    const int views =
        renderer ? *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(renderer) + kRendererViewCount) : 0;
    if (views != 4) {  // another family (scene captures, the spectator): not measured
        originalRenderFamily(rhiCmdList, renderer);
        return;
    }
    {
        std::lock_guard guard(mutex);
        ++familyFrames;
        const int ab = config.ab_frames();
        renderState = ab > 0 ? static_cast<int>((familyFrames / ab) % 2) : 0;
        inMainFamily = true;
    }
    const std::int64_t t0 = now();
    originalRenderFamily(rhiCmdList, renderer);
    const std::int64_t t1 = now();
    std::lock_guard guard(mutex);
    inMainFamily = false;
    StateStat& s = stats[renderState];
    ++s.frames;
    s.renderTicks += t1 - t0;
    if (!firstFrameLogged && familyFrames == 2) {
        firstFrameLogged = true;
        simqvfr::logf("acc passes: one frame's mesh pass setups (stereo pass/pass type:draws):%s", firstFrame.c_str());
    }
}

template <typename Fn>
bool hook(std::uintptr_t rva, const std::uint8_t* anchor, std::size_t size, void* detour, Fn& original, const char* name) {
    if (!simqvfr::code_matches(rva, anchor, size)) {
        simqvfr::logf("acc passes: %s at 0x%llx doesn't match this build: probe off", name,
                      static_cast<unsigned long long>(rva));
        return false;
    }
    void* target = reinterpret_cast<void*>(simqvfr::module_base() + rva);
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) != MH_OK || MH_EnableHook(target) != MH_OK) {
        simqvfr::logf("acc passes: hooking %s failed: probe off", name);
        return false;
    }
    return true;
}

double ms(std::int64_t ticks, std::uint64_t frames) {
    return frames && qpcFrequency ? 1000.0 * static_cast<double>(ticks) / static_cast<double>(qpcFrequency) /
                                        static_cast<double>(frames)
                                  : 0.0;
}

}  // namespace

PassProbeConfig pass_probe_config(const wchar_t* ini) {
    PassProbeConfig c;
    if (!ini)
        return c;
    c.probe = GetPrivateProfileIntW(L"experiment", L"acc_pass_probe", 0, ini) != 0;
    c.skipAbFrames = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"acc_focus_skip_ab", 0, ini));
    if (c.skipAbFrames < 0)
        c.skipAbFrames = 0;
    c.gatherAbFrames = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"acc_gather_skip_ab", 0, ini));
    if (c.gatherAbFrames < 0)
        c.gatherAbFrames = 0;
    return c;
}

bool pass_probe_start(const PassProbeConfig& c) {
    if (!c.on() || installed.load())
        return installed.load();
    config = c;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFrequency = f.QuadPart;
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        simqvfr::log_line("acc passes: MH_Initialize failed: probe off");
        return false;
    }
    // Setup first: the A/B's skip lives there; the timing hooks after it.
    if (!hook(kDispatchPassSetup, kDispatchPassSetupAnchor, sizeof(kDispatchPassSetupAnchor),
              reinterpret_cast<void*>(&setup_hook), originalSetup, "DispatchPassSetup") ||
        !hook(kDispatchDraw, kDispatchDrawAnchor, sizeof(kDispatchDrawAnchor), reinterpret_cast<void*>(&draw_hook),
              originalDraw, "DispatchDraw") ||
        !hook(kInitViews, kInitViewsAnchor, sizeof(kInitViewsAnchor), reinterpret_cast<void*>(&init_views_hook),
              originalInitViews, "InitViews") ||
        !hook(kRenderViewFamily, kRenderViewFamilyAnchor, sizeof(kRenderViewFamilyAnchor),
              reinterpret_cast<void*>(&render_family_hook), originalRenderFamily, "RenderViewFamily_RenderThread"))
        return false;
    // The visibility timing: with acc_pass_probe=1, and for the gather A/B.
    if ((c.probe || c.gather_ab()) &&
        (!hook(kComputeViewVisibility, kComputeViewVisibilityAnchor, sizeof(kComputeViewVisibilityAnchor),
               reinterpret_cast<void*>(&compute_visibility_hook), originalComputeVisibility, "ComputeViewVisibility") ||
         !hook(kPrecomputedVisibility, kPrecomputedVisibilityAnchor, sizeof(kPrecomputedVisibilityAnchor),
               reinterpret_cast<void*>(&precomputed_hook), originalPrecomputed, "precomputed visibility") ||
         !hook(kViewCullStep, kViewCullStepAnchor, sizeof(kViewCullStepAnchor), reinterpret_cast<void*>(&cull_step_hook),
               originalCullStep, "the view cull step") ||
         !hook(kOcclusionCull, kOcclusionCullAnchor, sizeof(kOcclusionCullAnchor), reinterpret_cast<void*>(&occlusion_hook),
               originalOcclusion, "OcclusionCull") ||
         !hook(kComputeRelevance, kComputeRelevanceAnchor, sizeof(kComputeRelevanceAnchor),
               reinterpret_cast<void*>(&relevance_hook), originalRelevance, "relevance") ||
         !hook(kGatherDynamicMeshElements, kGatherDynamicMeshElementsAnchor, sizeof(kGatherDynamicMeshElementsAnchor),
               reinterpret_cast<void*>(&gather_hook), originalGather, "GatherDynamicMeshElements") ||
         !hook(kSetupMeshPass, kSetupMeshPassAnchor, sizeof(kSetupMeshPassAnchor),
               reinterpret_cast<void*>(&setup_mesh_pass_hook), originalSetupMeshPass, "SetupMeshPass")))
        simqvfr::log_line("acc passes: visibility timing off (the mesh pass probe stays)");
    installed.store(true);
    std::string ab;
    if (c.skipAbFrames > 0)
        ab = "A/B: the focus pair's non-translucent mesh passes skipped every other " + std::to_string(c.skipAbFrames) +
             " frames";
    else if (c.gatherAbFrames > 0)
        ab = "A/B: the focus views left out of the dynamic mesh gather every other " +
             std::to_string(c.gatherAbFrames) + " frames";
    simqvfr::logf("acc passes: probe on (%s%s%s)", c.probe ? "log" : "", c.probe && !ab.empty() ? ", " : "", ab.c_str());
    return true;
}

void pass_probe_frame_end() {
    if (!installed.load(std::memory_order_relaxed) || ++reportFrames % 900 != 0)
        return;
    StateStat s[kStates];
    {
        std::lock_guard guard(mutex);
        for (int i = 0; i < kStates; ++i) {
            s[i] = stats[i];
            stats[i] = StateStat{};
        }
    }
    const char* names[kStates] = {"all drawn", config.gather_ab() ? "focus views out of the dynamic mesh gather"
                                                                  : "focus pair's opaque mesh passes skipped"};
    for (int st = 0; st < kStates; ++st) {
        if (!s[st].frames)
            continue;
        std::string line;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "acc passes: %s, %llu frames: scene render %.3f ms/frame, InitViews %.3f;",
                      names[st], static_cast<unsigned long long>(s[st].frames), ms(s[st].renderTicks, s[st].frames),
                      ms(s[st].initViewsTicks, s[st].frames));
        line = buf;
        for (int stereo = 0; stereo < kStereoPasses; ++stereo)
            for (int type = 0; type < kTypes; ++type) {
                const PassStat& p = s[st].pass[stereo][type];
                if (!p.setups && !p.dispatches)
                    continue;
                std::snprintf(buf, sizeof(buf), " %d/%d draws %.0f dispatch %.3f ms%s%s;", stereo, type,
                              static_cast<double>(p.draws) / static_cast<double>(s[st].frames),
                              ms(p.dispatchTicks, s[st].frames), p.parallel ? " parallel" : "",
                              p.skipped ? " skipped" : "");
                line += buf;
            }
        simqvfr::log_line(line.c_str());
        if (s[st].visibilityTicks) {
            std::snprintf(buf, sizeof(buf), "acc passes: visibility, %s: ComputeViewVisibility %.3f ms/frame, dynamic "
                          "mesh gather (all views) %.3f;",
                          names[st], ms(s[st].visibilityTicks, s[st].frames), ms(s[st].gatherTicks, s[st].frames));
            line = buf;
            for (int stereo = 0; stereo < kStereoPasses; ++stereo) {
                std::int64_t total = 0;
                for (int k = 0; k < kVisStages; ++k)
                    total += s[st].visTicks[stereo][k];
                if (!total)
                    continue;
                std::snprintf(buf, sizeof(buf), " view %d %.3f (", stereo, ms(total, s[st].frames));
                line += buf;
                for (int k = 0; k < kVisStages; ++k) {
                    std::snprintf(buf, sizeof(buf), "%s%s %.3f", k ? ", " : "", kVisStageNames[k],
                                  ms(s[st].visTicks[stereo][k], s[st].frames));
                    line += buf;
                }
                line += ");";
            }
            simqvfr::log_line(line.c_str());
        }
    }
    if (s[0].frames && s[1].frames)
        simqvfr::logf("acc passes: A/B: scene render %.3f ms (all drawn) vs %.3f ms (%s): %+.3f ms/frame; InitViews "
                      "%.3f vs %.3f; ComputeViewVisibility %.3f vs %.3f; dynamic mesh gather %.3f vs %.3f (%+.3f)",
                      ms(s[0].renderTicks, s[0].frames), ms(s[1].renderTicks, s[1].frames), names[1],
                      ms(s[1].renderTicks, s[1].frames) - ms(s[0].renderTicks, s[0].frames),
                      ms(s[0].initViewsTicks, s[0].frames), ms(s[1].initViewsTicks, s[1].frames),
                      ms(s[0].visibilityTicks, s[0].frames), ms(s[1].visibilityTicks, s[1].frames),
                      ms(s[0].gatherTicks, s[0].frames), ms(s[1].gatherTicks, s[1].frames),
                      ms(s[1].gatherTicks, s[1].frames) - ms(s[0].gatherTicks, s[0].frames));
}

}  // namespace acc
