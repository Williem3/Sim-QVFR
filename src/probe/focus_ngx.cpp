#include "focus_ngx.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <algorithm>
#include <string>
#include <vector>

#include <MinHook.h>

#include "../log.h"

#if defined(SIM_QVFR_NGX)
#include <nvsdk_ngx_params.h>
#endif

namespace simqvfr::focus_ngx {

#if !defined(SIM_QVFR_NGX)

// Built without NVIDIA's NGX headers: no focus DLSS.
bool install() { return false; }
bool installed() { return false; }
void preload() {}
void set_presets(int, int) {}
int focus_preset() { return 0; }
void set_options(float, bool) {}
float jitter_x() { return 1; }
State state() { return {}; }
void note_focus_running(bool) {}
bool latest(int, EyeEval&) { return false; }
CreateInfo create_info() { return {}; }
uint64_t game_evaluations() { return 0; }
struct Feature {};
Feature* make(ID3D12GraphicsCommandList*, unsigned, unsigned, int, unsigned, unsigned) { return nullptr; }
void release(Feature*) {}
bool alive(const Feature*) { return false; }
void set_eye_live(uint64_t) {}
void set_eye_live_out(uint64_t) {}
bool evaluate(ID3D12GraphicsCommandList*, Feature*, const Inputs&) { return false; }

#else

namespace {

using Bag = NVSDK_NGX_Parameter;
using CreateFn = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, Bag*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const Bag*, void*);
using AllocateFn = NVSDK_NGX_Result (*)(Bag**);
using DestroyFn = NVSDK_NGX_Result (*)(Bag*);
using ReleaseFn = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*);

CreateFn createOriginal = nullptr;
EvaluateFn evaluateOriginal = nullptr;
AllocateFn allocateFn = nullptr;
DestroyFn destroyFn = nullptr;
ReleaseFn releaseFn = nullptr;
// NGX's lifetime as the game runs it. A shutdown (the game's, on a change of its upscaling settings) frees every
// feature and parameter bag made on it, ours too: our handles from before are dead and must not be released or
// evaluated again (06:23 run: NVSDK_NGX_D3D12_ReleaseFeature read a freed handle, an access violation, 60 ms after a
// DLAA -> Performance change). `generation` counts the shutdowns; lifeLock is held shared around our own NGX calls.
using ShutdownFn = NVSDK_NGX_Result (*)();
using Shutdown1Fn = NVSDK_NGX_Result (*)(ID3D12Device*);
ShutdownFn shutdownOriginal = nullptr;
Shutdown1Fn shutdown1Original = nullptr;
ReleaseFn releaseOriginal = nullptr;  // the game's releases, seen
SRWLOCK lifeLock = SRWLOCK_INIT;
std::atomic<uint64_t> generation{1};
std::atomic<int> releasesLogged{0}, staleLogged{0};
std::atomic<bool> hooked{false};
bool tried = false;
SRWLOCK lock = SRWLOCK_INIT;
CreateInfo made;
EyeEval eyes[2];
std::atomic<uint64_t> evaluations{0};
int logged = 0;  // whole evaluations logged
std::atomic<int> eyePreset{0}, focusPreset{0};  // set_presets: 0 the game's own
std::atomic<float> jitterX{1.0f};        // set_options
std::atomic<bool> peripheryDlaa{false};  // set_options
std::atomic<bool> forcedDlaa{false}, focusRunning{false};
using OptimalFn = NVSDK_NGX_Result (*)(Bag*);
using ParametersFn = NVSDK_NGX_Result (*)(Bag**);
OptimalFn optimalOriginal = nullptr;
ParametersFn capabilityOriginal = nullptr;
std::atomic<bool> optimalHooked{false};
std::atomic<int> optimalLogged{0};

// The game's own SuperSampling features (its eyes), as made: what a replacement with another preset needs.
struct GameFeature {
    const NVSDK_NGX_Handle* game = nullptr;
    CreateInfo info;            // as the game asked
    int quality = 0;            // as made (DLAA when forced)
    unsigned creationMask = 1, visibilityMask = 1;
    int subrects = 0;
    bool hasSubrects = false;
    int preset = 0;             // the preset override it runs with now (0: the game's own)
    NVSDK_NGX_Handle* ours = nullptr;  // our replacement (null: the game's own feature runs)
    int basePreset = 0;         // the override the game's own feature was made with
    // the live periphery: our feature at the size the eye is drawn now
    NVSDK_NGX_Handle* live = nullptr;
    unsigned liveW = 0, liveH = 0, liveOutW = 0, liveOutH = 0;
    int livePreset = 0;
};
std::vector<GameFeature> gameFeatures;
std::atomic<uint64_t> eyeLiveState{0};  // set_eye_live
std::atomic<uint64_t> eyeLiveOut{0};    // set_eye_live_out

// The live region at the output size of a feature with output `outW` x `outH`: the probe's whole-block one when it was
// worked out for this output, else the output x the render size's share.
void live_output(unsigned outW, unsigned outH, double fx, double fy, unsigned& ow, unsigned& oh) {
    const uint64_t o = eyeLiveOut.load(std::memory_order_relaxed);
    if (o && ((o >> 32) & 0xffff) == outW && (o >> 48) == outH) {
        ow = static_cast<unsigned>(o & 0xffff);
        oh = static_cast<unsigned>((o >> 16) & 0xffff);
        return;
    }
    ow = static_cast<unsigned>(std::lround(outW * fx));
    oh = static_cast<unsigned>(std::lround(outH * fy));
}
// Features of ours that were replaced: released a few evaluations later (the GPU may still be running their last one).
struct OldFeature {
    NVSDK_NGX_Handle* handle;
    uint64_t generation, due;
};
std::vector<OldFeature> oldFeatures;  // under `lock`

const char* const kPresetKeys[6] = {
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};

bool ok(NVSDK_NGX_Result r) { return (static_cast<unsigned>(r) & 0xfff00000u) != 0xbad00000u; }

std::string name_of(ID3D12Resource* r) {
    if (!r)
        return "-";
    wchar_t name[96] = {};
    UINT size = sizeof(name) - sizeof(wchar_t);
    if (FAILED(r->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)) || !name[0])
        return "unnamed";
    std::string out;
    for (const wchar_t* c = name; *c; ++c)
        out += (std::iswalnum(*c) || *c == L'_' || *c == L'.') ? static_cast<char>(*c) : '_';
    return out;
}

std::string describe(ID3D12Resource* r) {
    if (!r)
        return "-";
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s %llux%u fmt %d x%u", name_of(r).c_str(), static_cast<unsigned long long>(d.Width),
                  d.Height, static_cast<int>(d.Format), d.SampleDesc.Count);
    return buf;
}

ID3D12Resource* get_res(const Bag* b, const char* k) {
    ID3D12Resource* r = nullptr;
    return ok(b->Get(k, &r)) ? r : nullptr;
}
unsigned get_ui(const Bag* b, const char* k) {
    unsigned v = 0;
    return ok(b->Get(k, &v)) ? v : 0;
}
int get_i(const Bag* b, const char* k, bool* has = nullptr) {
    int v = 0;
    const bool h = ok(b->Get(k, &v));
    if (has)
        *has = h;
    return h ? v : 0;
}
float get_f(const Bag* b, const char* k, bool* has = nullptr) {
    float v = 0;
    const bool h = ok(b->Get(k, &v));
    if (has)
        *has = h;
    return h ? v : 0;
}

// NGX's DLSS optimal-settings query (the callback a parameter bag hands out): the game asks it for its render size in
// the chosen mode. With peripheryDlaa the answer is render = output for every mode, so the game draws its eyes at full
// size and its feature (made as DLAA in createHook) only antialiases.
NVSDK_NGX_Result optimalHook(Bag* p) {
    const NVSDK_NGX_Result r = optimalOriginal(p);
    if (!p || !ok(r))
        return r;
    const unsigned w = get_ui(p, NVSDK_NGX_Parameter_Width), h = get_ui(p, NVSDK_NGX_Parameter_Height);
    const unsigned ow = get_ui(p, NVSDK_NGX_Parameter_OutWidth), oh = get_ui(p, NVSDK_NGX_Parameter_OutHeight);
    const int quality = get_i(p, NVSDK_NGX_Parameter_PerfQualityValue);
    const bool force = peripheryDlaa.load() && w && h && ow && oh && (ow != w || oh != h);
    if (optimalLogged.fetch_add(1) < 12)
        logf("focus dlss: the game asked NGX's optimal settings: target %ux%u, quality %d -> render %ux%u%s", w, h, quality,
             ow, oh, force ? "; answered render = target (periphery DLAA)" : "");
    if (force) {
        p->Set(NVSDK_NGX_Parameter_OutWidth, w);
        p->Set(NVSDK_NGX_Parameter_OutHeight, h);
        p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, w);
        p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, h);
        p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, w);
        p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, h);
    }
    return r;
}

// A parameter bag NGX hands the game: the optimal-settings callback it carries, detoured once.
void hook_optimal(Bag* bag) {
    if (!bag || optimalHooked.load())
        return;
    void* callback = nullptr;
    if (!ok(bag->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback)) || !callback)
        return;
    if (optimalHooked.exchange(true))
        return;
    MH_STATUS s = MH_CreateHook(callback, reinterpret_cast<void*>(&optimalHook), reinterpret_cast<void**>(&optimalOriginal));
    if (s == MH_OK)
        s = MH_EnableHook(callback);
    logf("focus dlss: NGX's DLSS optimal-settings query %s", s == MH_OK ? "detoured" : MH_StatusToString(s));
}

NVSDK_NGX_Result capabilityHook(Bag** out) {
    const NVSDK_NGX_Result r = capabilityOriginal(out);
    if (ok(r) && out)
        hook_optimal(*out);
    return r;
}

NVSDK_NGX_Result allocateHook(Bag** out) {
    const NVSDK_NGX_Result r = allocateFn(out);
    if (ok(r) && out)
        hook_optimal(*out);
    return r;
}

NVSDK_NGX_Result createHook(ID3D12GraphicsCommandList* l, NVSDK_NGX_Feature feature, Bag* bag, NVSDK_NGX_Handle** out) {
    if (feature == NVSDK_NGX_Feature_SuperSampling && bag) {
        CreateInfo c;
        c.seen = true;
        c.width = get_ui(bag, NVSDK_NGX_Parameter_Width);
        c.height = get_ui(bag, NVSDK_NGX_Parameter_Height);
        c.outWidth = get_ui(bag, NVSDK_NGX_Parameter_OutWidth);
        c.outHeight = get_ui(bag, NVSDK_NGX_Parameter_OutHeight);
        c.quality = get_i(bag, NVSDK_NGX_Parameter_PerfQualityValue);
        c.flags = get_i(bag, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
        for (int i = 0; i < 6; ++i) {
            unsigned v = 0;
            c.presetSet[i] = ok(bag->Get(kPresetKeys[i], &v));
            c.presets[i] = v;
        }
        AcquireSRWLockExclusive(&lock);
        made = c;
        ReleaseSRWLockExclusive(&lock);
        logf("focus dlss: the game made a DLSS feature: render %ux%u, output %ux%u, quality %d, create flags 0x%x, "
             "presets DLAA %u Q %u B %u P %u UP %u UQ %u",
             c.width, c.height, c.outWidth, c.outHeight, c.quality, c.flags, c.presets[0], c.presets[1], c.presets[2],
             c.presets[3], c.presets[4], c.presets[5]);
        if (c.width != c.outWidth || c.height != c.outHeight)
            logf("focus dlss: the game's mode upscales its eyes (%ux%u -> %ux%u): the periphery is not DLAA in this mode; the "
                 "focus views are drawn from its render-size targets",
                 c.width, c.height, c.outWidth, c.outHeight);
        // The periphery at DLAA in every in-game mode: with the optimal-settings query answered render = output, the
        // game's feature is made as DLAA (`made` keeps the game's own mode).
        forcedDlaa = false;
        if (peripheryDlaa.load() && c.quality != static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA)) {
            if (c.width == c.outWidth && c.height == c.outHeight) {
                bag->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA));
                forcedDlaa = true;
                logf("focus dlss: the game's feature (in-game mode %d) made as DLAA: the periphery at %ux%u", c.quality,
                     c.width, c.height);
            } else {
                log_line("focus dlss: periphery DLAA asked, but the game still renders its eyes smaller than its output in "
                         "this mode (its render size does not come from NGX's optimal-settings query)");
            }
        }
        // The eyes' (the periphery's) preset override: the game's feature made with ours for every mode (the one change
        // to the game's NGX calls; `made` keeps the game's own for our focus features).
        if (const int preset = eyePreset.load(); preset > 0) {
            for (const char* key : kPresetKeys)
                bag->Set(key, static_cast<unsigned>(preset));
            logf("focus dlss: the game's feature (the eyes, the periphery) made with preset %d instead of its own", preset);
        }
        GameFeature g;
        g.info = c;
        g.quality = forcedDlaa.load() ? static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA) : c.quality;
        if (const unsigned m = get_ui(bag, NVSDK_NGX_Parameter_CreationNodeMask))
            g.creationMask = m;
        if (const unsigned m = get_ui(bag, NVSDK_NGX_Parameter_VisibilityNodeMask))
            g.visibilityMask = m;
        g.subrects = get_i(bag, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, &g.hasSubrects);
        g.preset = g.basePreset = eyePreset.load();
        const NVSDK_NGX_Result r = createOriginal(l, feature, bag, out);
        if (ok(r) && out && *out) {
            g.game = *out;
            AcquireSRWLockExclusive(&lock);
            std::erase_if(gameFeatures, [&](const GameFeature& f) { return f.game == g.game; });  // a handle's address reused
            if (gameFeatures.size() >= 8)
                gameFeatures.erase(gameFeatures.begin());
            gameFeatures.push_back(g);
            ReleaseSRWLockExclusive(&lock);
        }
        return r;
    }
    return createOriginal(l, feature, bag, out);
}

// The eyes' preset changed while running: a feature of ours like the game's with that preset, recorded into the game's
// list ahead of its evaluation. The game's own feature stays (it releases it itself); an older replacement is left to
// NGX's shutdown, like our focus features on a size change.
const NVSDK_NGX_Handle* eye_feature(ID3D12GraphicsCommandList* l, const NVSDK_NGX_Handle* h, bool& fresh) {
    const int want = eyePreset.load();
    GameFeature g;
    {
        AcquireSRWLockShared(&lock);
        const auto at = std::find_if(gameFeatures.begin(), gameFeatures.end(), [&](const GameFeature& f) { return f.game == h; });
        const bool found = at != gameFeatures.end();
        if (found)
            g = *at;
        ReleaseSRWLockShared(&lock);
        if (!found)
            return h;
    }
    if (g.preset == want)
        return g.ours ? g.ours : h;
    NVSDK_NGX_Handle* created = nullptr;
    if (want != g.basePreset) {
        Bag* b = nullptr;
        NVSDK_NGX_Result r = allocateFn(&b);
        if (ok(r) && b) {
            b->Set(NVSDK_NGX_Parameter_CreationNodeMask, g.creationMask);
            b->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, g.visibilityMask);
            b->Set(NVSDK_NGX_Parameter_Width, g.info.width);
            b->Set(NVSDK_NGX_Parameter_Height, g.info.height);
            b->Set(NVSDK_NGX_Parameter_OutWidth, g.info.outWidth);
            b->Set(NVSDK_NGX_Parameter_OutHeight, g.info.outHeight);
            b->Set(NVSDK_NGX_Parameter_PerfQualityValue, g.quality);
            b->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, g.info.flags);
            if (g.hasSubrects)
                b->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, g.subrects);
            for (int i = 0; i < 6; ++i) {
                if (want > 0)
                    b->Set(kPresetKeys[i], static_cast<unsigned>(want));
                else if (g.info.presetSet[i])
                    b->Set(kPresetKeys[i], g.info.presets[i]);
            }
            r = createOriginal(l, NVSDK_NGX_Feature_SuperSampling, b, &created);
            destroyFn(b);
        }
        logf("focus dlss: the eyes' (periphery's) feature %ux%u remade with preset %d (0: the game's own): result 0x%08x%s",
             g.info.width, g.info.height, want, static_cast<unsigned>(r), ok(r) && created ? "" : "; the feature before stays");
        if (!ok(r) || !created)
            created = g.ours;  // keep what ran; not tried again until the preset changes
        else
            fresh = true;
    } else {
        logf("focus dlss: the eyes' (periphery's) preset back to %d: the game's own feature runs again", want);
        fresh = true;
    }
    AcquireSRWLockExclusive(&lock);
    for (GameFeature& f : gameFeatures)
        if (f.game == h) {
            f.preset = want;
            f.ours = created;
        }
    ReleaseSRWLockExclusive(&lock);
    return created ? created : h;
}

bool guarded_release(NVSDK_NGX_Handle* handle, Bag* bag);

// Under no lock: a replaced feature of ours goes once `evaluations` has passed its due count.
void retire_feature_later(NVSDK_NGX_Handle* handle) {
    if (!handle)
        return;
    AcquireSRWLockExclusive(&lock);
    oldFeatures.push_back({handle, generation.load(), evaluations.load() + 16});
    ReleaseSRWLockExclusive(&lock);
}
void release_old_features() {
    std::vector<OldFeature> due;
    AcquireSRWLockExclusive(&lock);
    const uint64_t now = evaluations.load();
    for (auto it = oldFeatures.begin(); it != oldFeatures.end();)
        if (it->due <= now) {
            due.push_back(*it);
            it = oldFeatures.erase(it);
        } else {
            ++it;
        }
    ReleaseSRWLockExclusive(&lock);
    if (due.empty())
        return;
    AcquireSRWLockShared(&lifeLock);
    for (const OldFeature& o : due)
        if (o.generation == generation.load() && !guarded_release(o.handle, nullptr))
            logf("focus dlss: NGX faulted releasing a replaced feature (handle %p): caught", static_cast<const void*>(o.handle));
    ReleaseSRWLockShared(&lifeLock);
}

// The live periphery: a feature of ours like the game's `h`, at the size its eye is drawn now (the game's sizes x fx,
// fy), recorded into the game's list. Null when the game's feature isn't known or NGX refuses (the game's own
// evaluation then runs unchanged).
const NVSDK_NGX_Handle* live_feature(ID3D12GraphicsCommandList* l, const NVSDK_NGX_Handle* h, double fx, double fy, bool& fresh) {
    const int want = eyePreset.load();
    GameFeature g;
    {
        AcquireSRWLockShared(&lock);
        const auto at = std::find_if(gameFeatures.begin(), gameFeatures.end(), [&](const GameFeature& f) { return f.game == h; });
        const bool found = at != gameFeatures.end();
        if (found)
            g = *at;
        ReleaseSRWLockShared(&lock);
        if (!found)
            return nullptr;
    }
    const unsigned w = static_cast<unsigned>(std::lround(g.info.width * fx)), hgt = static_cast<unsigned>(std::lround(g.info.height * fy));
    unsigned ow = 0, oh = 0;
    live_output(g.info.outWidth, g.info.outHeight, fx, fy, ow, oh);
    if (g.live && g.liveW == w && g.liveH == hgt && g.liveOutW == ow && g.liveOutH == oh && g.livePreset == want)
        return g.live;
    if (g.liveW == w && g.liveH == hgt && g.liveOutW == ow && g.liveOutH == oh && g.livePreset == want)
        return nullptr;  // refused at this size before: not tried again until it changes
    NVSDK_NGX_Handle* created = nullptr;
    Bag* b = nullptr;
    AcquireSRWLockShared(&lifeLock);
    NVSDK_NGX_Result r = allocateFn(&b);
    if (ok(r) && b) {
        b->Set(NVSDK_NGX_Parameter_CreationNodeMask, g.creationMask);
        b->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, g.visibilityMask);
        b->Set(NVSDK_NGX_Parameter_Width, w);
        b->Set(NVSDK_NGX_Parameter_Height, hgt);
        b->Set(NVSDK_NGX_Parameter_OutWidth, ow);
        b->Set(NVSDK_NGX_Parameter_OutHeight, oh);
        b->Set(NVSDK_NGX_Parameter_PerfQualityValue, g.quality);
        b->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, g.info.flags);
        b->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);
        for (int i = 0; i < 6; ++i) {
            if (want > 0)
                b->Set(kPresetKeys[i], static_cast<unsigned>(want));
            else if (g.info.presetSet[i])
                b->Set(kPresetKeys[i], g.info.presets[i]);
        }
        r = createOriginal(l, NVSDK_NGX_Feature_SuperSampling, b, &created);
        destroyFn(b);
    }
    ReleaseSRWLockShared(&lifeLock);
    (ok(r) && created ? logd : logf)("focus dlss: live periphery: a feature of ours %ux%u -> %ux%u in place of the game's %ux%u -> %ux%u (preset %d, 0: "
         "the game's own): result 0x%08x%s",
         w, hgt, ow, oh, g.info.width, g.info.height, g.info.outWidth, g.info.outHeight, want, static_cast<unsigned>(r),
         ok(r) && created ? "" : "; the game's own evaluation runs on its whole eye");
    if (!ok(r))
        created = nullptr;
    NVSDK_NGX_Handle* old = nullptr;
    AcquireSRWLockExclusive(&lock);
    for (GameFeature& f : gameFeatures)
        if (f.game == h) {
            old = f.live;
            f.live = created;
            f.liveW = w;
            f.liveH = hgt;
            f.liveOutW = ow;
            f.liveOutH = oh;
            f.livePreset = want;
        }
    ReleaseSRWLockExclusive(&lock);
    retire_feature_later(old);
    fresh = fresh || created != nullptr;
    return created;
}

NVSDK_NGX_Result evaluateHook(ID3D12GraphicsCommandList* l, const NVSDK_NGX_Handle* h, const Bag* bag, void* callback) {
    release_old_features();
    if (bag) {
        EyeEval e;
        e.color = get_res(bag, NVSDK_NGX_Parameter_Color);
        e.motion = get_res(bag, NVSDK_NGX_Parameter_MotionVectors);
        if (e.color && e.motion) {  // a DLSS (SuperSampling) evaluation
            e.depth = get_res(bag, NVSDK_NGX_Parameter_Depth);
            e.output = get_res(bag, NVSDK_NGX_Parameter_Output);
            e.exposure = get_res(bag, NVSDK_NGX_Parameter_ExposureTexture);
            e.transparency = get_res(bag, NVSDK_NGX_Parameter_TransparencyMask);
            e.bias = get_res(bag, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask);
            e.colorName = name_of(e.color);
            e.depthName = name_of(e.depth);
            e.motionName = name_of(e.motion);
            e.outputName = name_of(e.output);
            e.exposureName = name_of(e.exposure);
            e.jitter[0] = get_f(bag, NVSDK_NGX_Parameter_Jitter_Offset_X);
            e.jitter[1] = get_f(bag, NVSDK_NGX_Parameter_Jitter_Offset_Y);
            bool has = false;
            e.motionScale[0] = get_f(bag, NVSDK_NGX_Parameter_MV_Scale_X, &has);
            if (!has)
                e.motionScale[0] = 1;
            e.motionScale[1] = get_f(bag, NVSDK_NGX_Parameter_MV_Scale_Y, &has);
            if (!has)
                e.motionScale[1] = 1;
            e.preExposure = get_f(bag, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &e.hasPreExposure);
            e.exposureScale = get_f(bag, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &e.hasExposureScale);
            e.sharpness = get_f(bag, NVSDK_NGX_Parameter_Sharpness);
            e.frameTime = get_f(bag, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &e.hasFrameTime);
            e.reset = get_i(bag, NVSDK_NGX_Parameter_Reset);
            e.renderW = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width);
            e.renderH = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height);
            e.colorBase[0] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X);
            e.colorBase[1] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y);
            e.depthBase[0] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);
            e.depthBase[1] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
            e.motionBase[0] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X);
            e.motionBase[1] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
            e.outputBase[0] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);
            e.outputBase[1] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);
            const uint64_t n = evaluations.fetch_add(1);
            e.sequence = n + 1;
            // The right eye: the colour subrect starts past x 0 (one double-wide target); with separate targets per eye
            // (both at 0) the order in the frame decides.
            const int eye = e.colorBase[0] > 0 ? 1 : (e.colorBase[0] == 0 && e.outputBase[0] > 0 ? 1 : 0);
            bool logIt = false;
            AcquireSRWLockExclusive(&lock);
            eyes[eye] = e;
            if (logged < 4) {
                ++logged;
                logIt = true;
            }
            ReleaseSRWLockExclusive(&lock);
            if (logIt) {
                logd("focus dlss: game evaluation %llu (eye %d): colour %s base %u,%u; depth %s base %u,%u; motion %s base "
                     "%u,%u scale %.4g,%.4g; output %s base %u,%u; render %ux%u",
                     static_cast<unsigned long long>(n), eye, describe(e.color).c_str(), e.colorBase[0], e.colorBase[1],
                     describe(e.depth).c_str(), e.depthBase[0], e.depthBase[1], describe(e.motion).c_str(),
                     e.motionBase[0], e.motionBase[1], e.motionScale[0], e.motionScale[1], describe(e.output).c_str(),
                     e.outputBase[0], e.outputBase[1], e.renderW, e.renderH);
                logd("focus dlss:   jitter %.4f,%.4f; exposure texture %s; pre-exposure %s%.4g; exposure scale %s%.4g; "
                     "sharpness %.3g; frame time %s%.3g ms; reset %d; transparency mask %s; bias mask %s",
                     e.jitter[0], e.jitter[1], describe(e.exposure).c_str(), e.hasPreExposure ? "" : "(unset) ",
                     e.preExposure, e.hasExposureScale ? "" : "(unset) ", e.exposureScale, e.sharpness,
                     e.hasFrameTime ? "" : "(unset) ", e.frameTime, e.reset, describe(e.transparency).c_str(),
                     describe(e.bias).c_str());
            }
            // The horizontal jitter NGX is told, as really drawn (set_options): the kept evaluation holds the game's.
            if (const float sx = jitterX.load(); sx != 1.0f)
                const_cast<Bag*>(bag)->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, e.jitter[0] * sx);
            bool fresh = false;
            // The live periphery: the eye is drawn smaller, into the top left of its targets: the regions NGX is given,
            // and a feature of ours of that size. (The kept evaluation `e` holds the game's own values.)
            if (const uint64_t live = eyeLiveState.load(std::memory_order_relaxed)) {
                const double fx = static_cast<double>(live & 0xffff) / static_cast<double>((live >> 32) & 0xffff);
                const double fy = static_cast<double>((live >> 16) & 0xffff) / static_cast<double>(live >> 48);
                if (const NVSDK_NGX_Handle* const mine = live_feature(l, h, fx, fy, fresh)) {
                    Bag* const b = const_cast<Bag*>(bag);
                    const auto X = [fx](unsigned v) { return static_cast<unsigned>(std::lround(v * fx)); };
                    const auto Y = [fy](unsigned v) { return static_cast<unsigned>(std::lround(v * fy)); };
                    b->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, X(e.colorBase[0]));
                    b->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, Y(e.colorBase[1]));
                    b->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, X(e.depthBase[0]));
                    b->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, Y(e.depthBase[1]));
                    b->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, X(e.motionBase[0]));
                    b->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, Y(e.motionBase[1]));
                    // the output's bases by the output region's own share (whole blocks when the game upscales)
                    const CreateInfo created = create_info();
                    unsigned ow = 0, oh = 0;
                    live_output(created.outWidth, created.outHeight, fx, fy, ow, oh);
                    const double ox = made.outWidth ? static_cast<double>(ow) / made.outWidth : fx;
                    const double oy = made.outHeight ? static_cast<double>(oh) / made.outHeight : fy;
                    b->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
                           static_cast<unsigned>(std::lround(e.outputBase[0] * ox)));
                    b->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,
                           static_cast<unsigned>(std::lround(e.outputBase[1] * oy)));
                    b->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, X(e.renderW));
                    b->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, Y(e.renderH));
                    if (fresh)
                        b->Set(NVSDK_NGX_Parameter_Reset, 1);
                    return evaluateOriginal(l, mine, bag, callback);
                }
            }
            const NVSDK_NGX_Handle* const use = eye_feature(l, h, fresh);
            if (fresh)  // another feature than last frame's: no history of its own
                const_cast<Bag*>(bag)->Set(NVSDK_NGX_Parameter_Reset, 1);
            return evaluateOriginal(l, use, bag, callback);
        }
    }
    return evaluateOriginal(l, h, bag, callback);
}

void forget_game_features(const char* which) {
    AcquireSRWLockExclusive(&lock);
    const size_t n = gameFeatures.size();
    gameFeatures.clear();
    oldFeatures.clear();
    ReleaseSRWLockExclusive(&lock);
    logf("focus dlss: the game shut NGX down (%s): every feature made before is gone, ours too (%zu of its own recorded); ours are "
         "made again, not released",
         which, n);
}

NVSDK_NGX_Result shutdownHook() {
    AcquireSRWLockExclusive(&lifeLock);
    generation.fetch_add(1);
    forget_game_features("NVSDK_NGX_D3D12_Shutdown");
    const NVSDK_NGX_Result r = shutdownOriginal();
    ReleaseSRWLockExclusive(&lifeLock);
    return r;
}
NVSDK_NGX_Result shutdown1Hook(ID3D12Device* device) {
    AcquireSRWLockExclusive(&lifeLock);
    generation.fetch_add(1);
    forget_game_features("NVSDK_NGX_D3D12_Shutdown1");
    const NVSDK_NGX_Result r = shutdown1Original(device);
    ReleaseSRWLockExclusive(&lifeLock);
    return r;
}
// The game's own releases: logged (the first few), and our record of that feature goes.
NVSDK_NGX_Result releaseHook(NVSDK_NGX_Handle* h) {
    AcquireSRWLockExclusive(&lock);
    const size_t gone = std::erase_if(gameFeatures, [&](const GameFeature& f) { return f.game == h; });
    ReleaseSRWLockExclusive(&lock);
    if (releasesLogged.fetch_add(1) < 16)
        logd("focus dlss: the game released a feature (%p; %s)", static_cast<const void*>(h),
             gone ? "one of its eyes'" : "not one we saw made");
    return releaseOriginal(h);
}

// releaseFn / destroyFn on a handle NGX may already have freed: an access violation there is caught and reported
// instead of taking the game down.
bool guarded_release(NVSDK_NGX_Handle* handle, Bag* bag) {
    __try {
        if (handle)
            releaseFn(handle);
        if (bag)
            destroyFn(bag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

struct Feature {
    NVSDK_NGX_Handle* handle = nullptr;
    Bag* bag = nullptr;
    uint64_t generation = 0;  // NGX's lifetime it was made in
};

bool installed() { return hooked.load(); }

bool install() {
    if (hooked.load())
        return true;
    HMODULE ngx = GetModuleHandleW(L"_nvngx.dll");
    if (!ngx)
        return false;
    AcquireSRWLockExclusive(&lock);
    if (tried) {
        ReleaseSRWLockExclusive(&lock);
        return hooked.load();
    }
    tried = true;
    ReleaseSRWLockExclusive(&lock);
    void* create = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
    void* evaluate = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
    allocateFn = reinterpret_cast<AllocateFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
    destroyFn = reinterpret_cast<DestroyFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_DestroyParameters"));
    releaseFn = reinterpret_cast<ReleaseFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
    void* const shutdown = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown"));
    void* const shutdown1 = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown1"));
    if (!create || !evaluate || !allocateFn || !destroyFn || !releaseFn) {
        log_line("focus dlss: _nvngx.dll lacks a D3D12 export (create / evaluate / parameters / release); no focus DLSS");
        return false;
    }
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        logf("focus dlss: MinHook: %s; no focus DLSS", MH_StatusToString(init));
        return false;
    }
    MH_STATUS s = MH_CreateHook(create, reinterpret_cast<void*>(&createHook), reinterpret_cast<void**>(&createOriginal));
    {  // the parameter bags NGX hands out (for the optimal-settings query): not needed for the rest
        void* const allocate = reinterpret_cast<void*>(allocateFn);
        void* const capability = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
        if (MH_CreateHook(allocate, reinterpret_cast<void*>(&allocateHook), reinterpret_cast<void**>(&allocateFn)) == MH_OK)
            MH_QueueEnableHook(allocate);
        if (capability && MH_CreateHook(capability, reinterpret_cast<void*>(&capabilityHook),
                                        reinterpret_cast<void**>(&capabilityOriginal)) == MH_OK)
            MH_QueueEnableHook(capability);
        // NGX's shutdowns and the game's releases: our features do not outlive a shutdown
        if (shutdown && MH_CreateHook(shutdown, reinterpret_cast<void*>(&shutdownHook),
                                      reinterpret_cast<void**>(&shutdownOriginal)) == MH_OK)
            MH_QueueEnableHook(shutdown);
        if (shutdown1 && MH_CreateHook(shutdown1, reinterpret_cast<void*>(&shutdown1Hook),
                                       reinterpret_cast<void**>(&shutdown1Original)) == MH_OK)
            MH_QueueEnableHook(shutdown1);
        void* const release = reinterpret_cast<void*>(releaseFn);
        if (MH_CreateHook(release, reinterpret_cast<void*>(&releaseHook), reinterpret_cast<void**>(&releaseOriginal)) == MH_OK) {
            releaseFn = releaseOriginal;  // our own releases go past the hook
            MH_QueueEnableHook(release);
        }
    }
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
        logf("focus dlss: could not detour NGX's exports (%s); no focus DLSS", MH_StatusToString(s));
        return false;
    }
    hooked = true;
    log_line("focus dlss: NGX's D3D12 create / evaluate exports detoured (_nvngx.dll)");
    return true;
}

void preload() {
    if (hooked.load() || GetModuleHandleW(L"_nvngx.dll")) {
        install();
        return;
    }
    wchar_t folder[MAX_PATH] = {};
    DWORD size = sizeof(folder);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
                     nullptr, folder, &size) != ERROR_SUCCESS) {
        log_line("focus dlss: no NGX core path registered by the driver; detours wait for the game's own load");
        return;
    }
    const std::wstring path = std::wstring(folder) + L"\\_nvngx.dll";
    if (!LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        logf("focus dlss: NGX core not loaded early (error %lu); detours wait for the game's own load", GetLastError());
        return;
    }
    if (install())
        log_line("focus dlss: NGX core loaded early from the driver's folder; the game's DLSS creation will be seen");
}

bool latest(int eye, EyeEval& out) {
    if (eye < 0 || eye > 1)
        return false;
    AcquireSRWLockShared(&lock);
    out = eyes[eye];
    ReleaseSRWLockShared(&lock);
    return out.sequence != 0;
}

CreateInfo create_info() {
    AcquireSRWLockShared(&lock);
    const CreateInfo c = made;
    ReleaseSRWLockShared(&lock);
    return c;
}

uint64_t game_evaluations() { return evaluations.load(); }

void set_presets(int eye, int focus) {
    const int eyeBefore = eyePreset.exchange(eye), focusBefore = focusPreset.exchange(focus);
    if (eyeBefore != eye || focusBefore != focus)
        logf("focus dlss: presets: the eyes (periphery) %d, the focus views %d (0: the game's own)", eye, focus);
}

int focus_preset() { return focusPreset.load(); }

void set_options(float jitter, bool dlaa) {
    jitterX.store(jitter);
    peripheryDlaa.store(dlaa);
}

float jitter_x() { return jitterX.load(); }

void note_focus_running(bool running) { focusRunning.store(running); }

State state() {
    const CreateInfo c = create_info();
    State s;
    s.seen = c.seen;
    s.forcedDlaa = forcedDlaa.load();
    s.gameQuality = c.quality;
    s.width = c.width;
    s.height = c.height;
    s.outWidth = c.outWidth;
    s.outHeight = c.outHeight;
    // the preset slot NGX reads: DLAA's when the feature is DLAA, else the mode's (the override fills them all)
    const int slot = (s.forcedDlaa || c.quality == static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA)) ? 0
                     : c.quality == static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality)         ? 1
                     : c.quality == static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced)           ? 2
                     : c.quality == static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxPerf)            ? 3
                     : c.quality == static_cast<int>(NVSDK_NGX_PerfQuality_Value_UltraPerformance)   ? 4
                                                                                                     : 5;
    s.eyePreset = eyePreset.load() > 0 ? eyePreset.load() : static_cast<int>(c.presets[slot]);
    s.focusPreset = focusPreset.load() > 0 ? focusPreset.load() : static_cast<int>(c.presets[0]);
    s.focusRunning = focusRunning.load();
    return s;
}

Feature* make(ID3D12GraphicsCommandList* list, unsigned width, unsigned height, int extraFlags, unsigned outWidth,
              unsigned outHeight) {
    if (!outWidth || !outHeight) {
        outWidth = width;
        outHeight = height;
    }
    // the mode by the ratio (NGX refuses UltraQuality: the LMU work), DLAA at render = output
    const double ratio = static_cast<double>(width) / outWidth;
    const int mode = (width == outWidth && height == outHeight) ? static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA)
                     : ratio >= 0.62                            ? static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality)
                     : ratio >= 0.54                            ? static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced)
                                                                : static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxPerf);
    if (!hooked.load())
        return nullptr;
    const CreateInfo c = create_info();
    auto* f = new Feature;
    AcquireSRWLockShared(&lifeLock);
    f->generation = generation.load();
    NVSDK_NGX_Result r = allocateFn(&f->bag);
    if (ok(r) && f->bag) {
        Bag* b = f->bag;
        b->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        b->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        b->Set(NVSDK_NGX_Parameter_Width, width);
        b->Set(NVSDK_NGX_Parameter_Height, height);
        b->Set(NVSDK_NGX_Parameter_OutWidth, outWidth);
        b->Set(NVSDK_NGX_Parameter_OutHeight, outHeight);
        // DLAA (render = output) or the mode by the ratio; the game's flags when its creation was seen, else HDR colour, reverse depth and
        // motion vectors at render size (ACE's targets: R11G11B10 linear, reverse-Z).
        b->Set(NVSDK_NGX_Parameter_PerfQualityValue, mode);
        const int flags = (c.seen ? c.flags
                                  : (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                     NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)) |
                          extraFlags;
        b->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
        b->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);
        const int ownPreset = focusPreset.load();  // the focus views' preset override (0: the game's presets)
        for (int i = 0; i < 6; ++i) {
            if (ownPreset > 0)
                b->Set(kPresetKeys[i], static_cast<unsigned>(ownPreset));
            else if (c.presetSet[i])
                b->Set(kPresetKeys[i], c.presets[i]);
        }
        r = createOriginal(list, NVSDK_NGX_Feature_SuperSampling, b, &f->handle);
        (ok(r) && f->handle ? logd : logf)("focus dlss: our feature %ux%u -> %ux%u (mode %d, flags 0x%x, preset %s%d, the game's creation %s): result 0x%08x",
             width, height, outWidth, outHeight, mode, flags, ownPreset > 0 ? "ours " : "the game's ", ownPreset > 0 ? ownPreset : static_cast<int>(c.presets[0]),
             c.seen ? "seen" : "not seen", static_cast<unsigned>(r));
    } else {
        logf("focus dlss: NGX parameters not allocated (0x%08x)", static_cast<unsigned>(r));
    }
    ReleaseSRWLockShared(&lifeLock);
    if (!ok(r) || !f->handle) {
        release(f);
        return nullptr;
    }
    return f;
}

void release(Feature* f) {
    if (!f)
        return;
    AcquireSRWLockShared(&lifeLock);
    if (f->generation != generation.load()) {  // NGX was shut down since: it freed the handle and the bag itself
        if (staleLogged.fetch_add(1) < 8)
            logd("focus dlss: a feature of ours from before NGX's shutdown is dropped, not released (NGX freed it)");
    } else if (releaseFn && destroyFn && !guarded_release(f->handle, f->bag)) {
        logf("focus dlss: NGX faulted releasing a feature of ours (handle %p): caught; the feature is left to NGX",
             static_cast<const void*>(f->handle));
    }
    ReleaseSRWLockShared(&lifeLock);
    delete f;
}

bool alive(const Feature* f) { return f && f->generation == generation.load(); }

void set_eye_live(uint64_t packed) { eyeLiveState.store(packed, std::memory_order_relaxed); }
void set_eye_live_out(uint64_t packed) { eyeLiveOut.store(packed, std::memory_order_relaxed); }

bool evaluate(ID3D12GraphicsCommandList* list, Feature* f, const Inputs& in) {
    if (!f || !f->bag || !f->handle || f->generation != generation.load())
        return false;
    Bag* b = f->bag;
    b->Set(NVSDK_NGX_Parameter_Color, in.color);
    b->Set(NVSDK_NGX_Parameter_Output, in.output);
    b->Set(NVSDK_NGX_Parameter_Depth, in.depth);
    b->Set(NVSDK_NGX_Parameter_MotionVectors, in.motion);
    b->Set(NVSDK_NGX_Parameter_ExposureTexture, in.exposure);
    b->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, in.jitter[0]);
    b->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, in.jitter[1]);
    b->Set(NVSDK_NGX_Parameter_MV_Scale_X, in.motionScale[0]);
    b->Set(NVSDK_NGX_Parameter_MV_Scale_Y, in.motionScale[1]);
    b->Set(NVSDK_NGX_Parameter_Reset, in.reset ? 1 : 0);
    for (const char* k : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X})
        b->Set(k, in.base[0]);
    b->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, in.ownOutBase ? in.outBase[0] : in.base[0]);
    b->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, in.ownOutBase ? in.outBase[1] : in.base[1]);
    for (const char* k : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
                          NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y})
        b->Set(k, in.base[1]);
    b->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, in.width);
    b->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, in.height);
    if (in.game) {
        if (in.game->hasPreExposure)
            b->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, in.game->preExposure);
        if (in.game->hasExposureScale)
            b->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, in.game->exposureScale);
        b->Set(NVSDK_NGX_Parameter_Sharpness, in.game->sharpness);
        if (in.game->hasFrameTime)
            b->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, in.game->frameTime);
    }
    const NVSDK_NGX_Result r = evaluateOriginal(list, f->handle, b, nullptr);
    static std::atomic<int> failures{0};
    if (!ok(r) && failures.fetch_add(1) < 3)
        logf("focus dlss: our evaluation refused: 0x%08x", static_cast<unsigned>(r));
    return ok(r);
}

#endif

}  // namespace simqvfr::focus_ngx
