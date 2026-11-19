#include "qvfr_frame.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "../../probe/d3d11_capture.h"
#include "../../probe/frame_parts.h"
#include "../../quad/eye_off.h"
#include "../../sha256_file.h"
#include "abi.h"
#include "../../engine/dlss_math.h"
#include "../../engine/dlss_views.h"
#include "../../engine/ngx_dlss.h"
#include "../../engine/nvapi_gpus.h"
#include "fixation.h"
#include "focus_math.h"
#include "game.h"
#include "mvr_pass.h"
#include "mvr_recovery.h"
#include "mvr_probe.h"
#include "qvfr_frame_internal.h"

namespace lmu {
namespace qvfr_frame_detail {

bool installed = false;
QvfrFrameRun cfg;           // the run's settings (qvfr_frame.h)
bool qvfrMode = false;       // QVFR: four passes every frame, composed (no samples)
// The frame capture hotkey (d3d11_capture_hotkey=1): the arm last acted on (capture_views), and as this frame's pass 0
// read it (pass_begin), so the views and the glare read back pass by pass are of one frame.
std::uint64_t seenCaptureArm = 0;
std::uint64_t frameCaptureArm = 0;
bool capture_due(std::uint64_t arm) { return arm != seenCaptureArm && arm != 0 && arm != 0xFFFFFFFFull; }

// The focus box (QVFR: live, from the settings and the gaze; under liveMutex): its share of the eye's tangent width and
// height (focusFractionY <= 0: as the width), and each eye's centre as tangents of its view (gazeValid false: the
// eye's forward axis).
std::mutex liveMutex;
double focusFraction = 0.3;
double focusFractionY = 0;
// The focus mask as last set (qvfr_frame_set_focus_mask), handed to mvr_pass at each frame's pass 0.
bool focusMaskOn = false;
simqvfr::FocusArea focusMaskArea{};
unsigned focusMaskMargin = 16;
float gazeTan[2][2] = {};
bool gazeValid[2] = {};

unsigned long long sceneFrames = 0;  // frames that rendered the 3D scene (the eye loop ran)

// QVFR: the mask's own FOV. VR_Init turns the runtime's mesh (tangents) into screen space with the FOV it just located,
// so the mask fits only that FOV: after a live FOV crop change it would black out what the lens shows. Under liveMutex,
// except maskStale (render thread reads).
float locatedTan[2][4] = {};  // the latest located FOV per eye: tangents left, right, down, up
bool locatedValid = false;
float maskTan[2][4] = {};
bool maskKnown[2] = {};
std::atomic<bool> maskStale{false};
bool maskStaleLogged = false;
int maskStaleThisFrame = 0;


// The render thread's view of the frame (pass_begin and frame_end both run on it; checked).
unsigned long renderThread = 0;
// xrEndFrame on another thread than the passes' (known once a pass ran): the immediate context is the render thread's,
// so the frame end's context work waits for a frame end on it and the atlas isn't made (the eyes submitted alone).
bool off_render_thread() { return renderThread && GetCurrentThreadId() != renderThread; }
int passesThisFrame = 0;
int eyeOfPass[4] = {-1, -1, -1, -1};
unsigned long long frames = 0;

std::uintptr_t base() { return simqvfr::module_base(); }

// ---- The game's facts on any build ----
// QVFR reads five facts of the game: the size it was offered, its render scale (its main targets' size), whether its FSR
// is on, each eye's projection and the eye a pass rendered. On a build abi.h's addresses are from (knownBuild) they are
// read there, as since the start; on any other build (a game update, the studio's beta or alpha branches) they come from
// what the layer sees instead (buildFree): the size it offered at xrEnumerateViewConfigurationViews, the scene target a
// pass binds, FSR's compute writes into the eye's image, the FOV the game was handed at xrLocateViews, and the passes'
// order. [experiment] lmu_build_free=1 takes them that way on a known build too (the check of this path). On a known
// build both are taken and compared (check_facts), so a run says whether the layer's agree with the game's.
bool knownBuild = false;
bool buildFree = false;
std::atomic<std::uint32_t> offeredW{0}, offeredH{0};  // the stereo views' size the layer offered the game
float gameFovTan[2][4] = {};  // the FOV the game was last handed (jitter included), tangents l r d u (under liveMutex)
bool gameFovValid = false;
bool fsrSeen = false;          // FSR's compute writes into an eye's image seen in the last frame with passes
int fsrSeenFrames = 0;         // frames in a row whose writes said otherwise (two change it)
std::string buildSha;

bool exe_known_build() {
    static const bool known = [] {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        buildSha = simqvfr::sha256_file(exe);
        return simqvfr::games::supported_build(kGame, buildSha);
    }();
    return known;
}

// The comparison of the two sources on a known build (the log): each fact's checks and mismatches, the first of each
// kind said with its values, a summary every 5400 checked frames (a minute at 90 Hz).
struct FactCheck {
    const char* name;
    unsigned long long checks = 0, mismatches = 0;
    int said = 0;
};
FactCheck factChecks[4] = {{"offered size"}, {"main targets' size"}, {"eye projection"}, {"FSR"}};
unsigned long long factFrames = 0;

void fact(int k, bool agree, const char* detail) {
    FactCheck& c = factChecks[k];
    ++c.checks;
    if (agree)
        return;
    ++c.mismatches;
    if (c.said < 3) {
        ++c.said;
        simqvfr::logf("lmu qvfr: frame %llu: build-free facts: the %s from the layer differs from the game's: %s%s", frames,
                      c.name, detail, buildFree ? " (the layer's taken)" : " (the game's taken)");
    }
}

void log_fact_checks(const char* when) {
    if (!knownBuild || factFrames == 0)
        return;
    char text[256];
    int n = 0;
    for (const FactCheck& c : factChecks)
        n += std::snprintf(text + n, sizeof(text) - n, "%s%s %llu of %llu", n ? ", " : "", c.name, c.checks - c.mismatches,
                           c.checks);
    simqvfr::logf("lmu qvfr: build-free facts %s: agree with the game's (%s)", when, text);
}

// The multi-view path's eye images, without calling the game's code: LMU's submit copies each eye's image into that eye's
// swapchain image (CopySubresourceRegion), so at xrEndFrame the copy into the submitted image names eye e's
// (learn_eye_images, mvr_pass.h mvr_copy_source). Held (AddRef'd) until another is seen. Null until the first.
ID3D11Texture2D* eyeImage[2] = {};
bool eyeImagesLogged = false, eyeImagesChecked = false;

void learn_eye_images(ID3D11Texture2D* const (&eyes)[2]) {
    for (int e = 0; e < 2; ++e) {
        ID3D11Resource* src = mvr_copy_source(eyes[e]);
        ID3D11Texture2D* t = nullptr;
        if (!src || FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))))
            continue;
        if (t == eyeImage[e]) {
            t->Release();
            continue;
        }
        if (eyeImage[e])
            eyeImage[e]->Release();
        eyeImage[e] = t;  // our reference
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        simqvfr::logf("lmu qvfr: frame %llu: eye %d's image %ux%u format %d, %s from its copy into the swapchain image "
                      "(no call into the game)",
                      frames, e, d.Width, d.Height, static_cast<int>(d.Format), eyeImagesLogged ? "again" : "found");
    }
    eyeImagesLogged = eyeImagesLogged || (eyeImage[0] && eyeImage[1]);
}

// The game's eye image e, AddRef'd (null if not known): on the multi-view path the one its copy named, else the game's.
ID3D11Texture2D* game_eye_image(int e) {
    if (eyeImage[e])
        eyeImage[e]->AddRef();
    return eyeImage[e];
}

// ---- Focus views ----

// The projection slots' 4x4 (focus_math.h): the camera update copies its eye's two into the camera each pass.
const float* game_slot(int eye, int kind) {
    return reinterpret_cast<const float*>(base() + (kind ? kEyeProjectionFar : kEyeProjection) + eye * kEyeSlotStride +
                                          kSlotMatrix);
}

// The same from the FOV the game was handed (any build): what the focus maths read of it (focus_math.h
// projection_from_tangents). False before the first.
bool layer_slot(int eye, float (&out)[16]) {
    float t[4];
    {
        std::lock_guard guard(liveMutex);
        if (!gameFovValid)
            return false;
        std::memcpy(t, gameFovTan[eye], sizeof(t));
    }
    return projection_from_tangents(t, out);
}

// Eye `eye`'s projection (kind 0; 1 its slot with a fixed far plane: the same tangents): the game's slot on a known
// build, the layer's on any other (both compared on a known build: kind 0, check_facts). False: none yet.
bool slot_matrix(int eye, int kind, float (&out)[16]) {
    if (buildFree)
        return layer_slot(eye, out);
    std::memcpy(out, game_slot(eye, kind), sizeof(out));
    return true;
}

FocusBox boxes[2];
bool shapeLogged = false;

// Both eyes' focus projections taken once at pass 0, so both eye passes and the frame's record use the same boxes (the
// gaze moves between them).
float frozenFocus[2][2][16];
FocusBox frozenBox[2];
ClipRemap frozenRemap[2];
bool frozen[2] = {};

bool focus_matrices(int eye, float (&eyeM)[2][16], float (&focusM)[2][16], FocusBox (&box)[2]);

// Eye `eye`'s projections (both slots) and its focus box's: from the live box and the gaze.
bool focus_matrices(int eye, float (&eyeM)[2][16], float (&focusM)[2][16], FocusBox (&box)[2]) {
    double fx, fy, cx = 0, cy = 0;
    {
        std::lock_guard guard(liveMutex);
        fx = focusFraction;
        fy = focusFractionY > 0 ? focusFractionY : focusFraction;
        if (gazeValid[eye]) {
            cx = gazeTan[eye][0];
            cy = gazeTan[eye][1];
        }
    }
    for (int k = 0; k < 2; ++k)
        if (!slot_matrix(eye, k, eyeM[k])) {  // a build-free run before the game was handed a FOV
            for (int j = 0; j < 2; ++j)
                box[j] = FocusBox{};
            return false;
        }
    // A share of the eye's tangent span: on the gaze, kept inside the eye (QVFR), else on the eye's forward axis (gates).
    clamp_focus_centre(eyeM[0], fx, fy, cx, cy);
    bool ok = true;
    for (int k = 0; k < 2; ++k)
        ok = focus_projection(eyeM[k], fx, fy, cx, cy, box[k], focusM[k]) && ok;
    // DLAA (engine/dlss_views.h): the eye's projection carries this frame's jitter and its DLAA output doesn't. The box's eye
    // tangents and place in the eye's image as the output has them: the frame's record (what the layer composes and
    // submits the periphery with) and the focus post's glare mapping read the output. The matrices stay as rendered.
    double shiftX, shiftY;
    if (ok && simqvfr::dlss_eye_clip_shift(eye, shiftX, shiftY))
        for (int k = 0; k < 2; ++k)
            simqvfr::dlss::unshift_box(box[k], shiftX, shiftY);
    if (!ok) {
        if (!shapeLogged) {
            shapeLogged = true;
            const float* m = eyeM[0];
            simqvfr::logf("lmu focus: eye %d's projection isn't the shape expected (m0 %g m5 %g m8 %g m9 %g m11 %g "
                          "m15 %g): the focus views render as copies",
                          eye, m[0], m[5], m[8], m[9], m[11], m[15]);
        }
        return false;
    }
    return true;
}

// MVR, at pass 0: both eyes' focus boxes for this frame (frozen until it ends).
void freeze_focus() {
    for (int e = 0; e < 2; ++e) {
        float eyeM[2][16];
        FocusBox box[2];
        frozen[e] = focus_matrices(e, eyeM, frozenFocus[e], box);
        if (frozen[e]) {
            frozenBox[e] = box[0];
            frozenRemap[e] = clip_remap(eyeM[0], frozenFocus[e][0]);
        }
    }
}

// ---- QVFR: the atlas ----
// All four views of a frame in one texture for the layer's compositor: the eyes in the top row (eye e at x = e W), their
// focus views below. Typeless: the game's images copy in, and the compositor reads it in the swapchain's format.
ID3D11Texture2D* atlas = nullptr;
unsigned viewW = 0, viewH = 0;  // every view's size (the eye targets')
DXGI_FORMAT atlasFormat = DXGI_FORMAT_UNKNOWN;
bool atlasReady = false;    // this frame's focus finals are both made, until the layer takes them into the atlas
unsigned long long composedFrames = 0, refusedFrames = 0;
bool atlasRefusalLogged = false, atlasThreadLogged = false, frameEndThreadLogged = false;

DXGI_FORMAT typeless_of(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default:
        return f;
    }
}

bool is_view(ID3D11Texture2D* t) {
    if (!t)
        return false;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    return d.Width == viewW && d.Height == viewH && typeless_of(d.Format) == atlasFormat && d.SampleDesc.Count == 1;
}

// The atlas for views the size and format of `eyeTexture` (made again when they change).
bool ensure_atlas(ID3D11Texture2D* eyeTexture) {
    D3D11_TEXTURE2D_DESC desc;
    eyeTexture->GetDesc(&desc);
    const DXGI_FORMAT format = typeless_of(desc.Format);
    if (atlas && viewW == desc.Width && viewH == desc.Height && atlasFormat == format)
        return true;
    if (atlas) {
        atlas->Release();
        atlas = nullptr;
    }
    viewW = viewH = 0;
    if (desc.SampleDesc.Count != 1 || desc.Width > 8192 || desc.Height > 8192) {
        simqvfr::logf("lmu qvfr: the eye target is %ux%u with %u samples: no atlas, not composed", desc.Width, desc.Height,
                      desc.SampleDesc.Count);
        return false;
    }
    D3D11_TEXTURE2D_DESC a{};
    a.Width = 2 * desc.Width;
    a.Height = 2 * desc.Height;
    a.MipLevels = 1;
    a.ArraySize = 1;
    a.Format = format;
    a.SampleDesc = {1, 0};
    a.Usage = D3D11_USAGE_DEFAULT;
    a.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device* device = nullptr;
    eyeTexture->GetDevice(&device);
    const HRESULT hr = device->CreateTexture2D(&a, nullptr, &atlas);
    device->Release();
    if (FAILED(hr)) {
        atlas = nullptr;
        simqvfr::logf("lmu qvfr: the atlas %ux%u (format %d) couldn't be made (0x%08lx): not composed", a.Width, a.Height,
                      static_cast<int>(format), static_cast<unsigned long>(hr));
        return false;
    }
    viewW = desc.Width;
    viewH = desc.Height;
    atlasFormat = format;
    simqvfr::logf("lmu qvfr: atlas %ux%u, format %d (views %ux%u, the eye targets' format %d)", a.Width, a.Height,
                  static_cast<int>(format), viewW, viewH, static_cast<int>(desc.Format));
    return true;
}

// ---- QVFR: live resolution ----
// Nothing is written into the game: it renders its eye passes at its own render size (its render scale, read), and
// mvr_pass.h mvr_size_cut cuts their viewports and scissors to the live eye size at the d3d11 level, the eye in the
// top-left of its launch-size targets, its focus view beside it (docs/recon-lmu.md "Census run 3": nothing else in the
// pass follows the size; YEBIS works on its whole targets at any size). The eye size is the game's own scale times the
// outer share (qvfr_frame_set_live_scales), as the game itself would size a pass at that scale.
// Two sizes per pass (focus_math.h final_extent): sceneW/sceneH, what it rendered of the main targets (the scene, the
// resolve, YEBIS: glare share and MVR's side-by-side viewports work there); passW/passH, the part of its eye target
// that holds its view after the final blit or FSR stretched the main target over it (the copies, the atlas regions, the
// driver tags and the VR UI). The same at the game's render scale 1; below it (FSR's presets) the eye target's part is
// larger by recommended / main. extentW/extentH: passW/passH unrounded, for the layer's record.
float liveScale[2] = {1, 1};  // outer, focus (under liveMutex)
float liveAxes[2][2] = {{1, 1}, {1, 1}};  // per axis (perAxis): outer, focus; width, height shares (under liveMutex)
float gameScale = 0;          // the game's own render scale (its setting; what the launch-size targets were made with)
unsigned passW[4] = {}, passH[4] = {};
unsigned sceneW[4] = {}, sceneH[4] = {};
double extentW[4] = {}, extentH[4] = {};
bool liveLogged = false, stretchLogged = false;
// The runtime's recommended size as the game took it (VR_Init): read in the game on a known build, else the size the
// layer offered it (the same: the game takes what it is offered).
void game_recommended(std::uint32_t (&out)[2]) {
    const auto* rec = reinterpret_cast<const std::uint32_t*>(base() + kRecommendedSize);
    out[0] = rec[0];
    out[1] = rec[1];
}
void real_recommended(std::uint32_t (&out)[2]) {
    if (buildFree) {
        out[0] = offeredW.load(std::memory_order_relaxed);
        out[1] = offeredH.load(std::memory_order_relaxed);
        return;
    }
    game_recommended(out);
}

// The main targets' size: the recommended size at the game's own render scale, as cubeDX11_CreateVRMainTargets makes
// them (build-free: the scene target the last pass began on, which is one of them); 0 x 0 before the game's scale is
// known.
void main_size(unsigned (&out)[2]) {
    out[0] = out[1] = 0;
    if (gameScale <= 0)
        return;
    if (buildFree) {
        mvr_scene_size(out[0], out[1]);
        return;
    }
    std::uint32_t rec[2];
    real_recommended(rec);
    out[0] = scaled_size(rec[0], gameScale);
    out[1] = scaled_size(rec[1], gameScale);
}

// The game's render scale: read on a known build; build-free, from its main targets' size over the recommended size
// (focus_math.h render_scale_from_sizes). 0: not known yet.
float game_render_scale() { return *reinterpret_cast<const float*>(base() + kRenderScale); }
float layer_render_scale() {
    unsigned m[2];
    std::uint32_t rec[2] = {offeredW.load(std::memory_order_relaxed), offeredH.load(std::memory_order_relaxed)};
    return mvr_scene_size(m[0], m[1]) ? render_scale_from_sizes(m[0], m[1], rec[0], rec[1]) : 0.0f;
}

// LMU's FSR 1 is on (its UpscaleMethod): each view's post-processing ends in FSR's upscale, not in the final blit.
// Build-free: FSR's compute writes into an eye's image seen (qvfr_frame_frame_end note_fsr), a frame late.
bool game_fsr_on() {
    const auto* renderer = *reinterpret_cast<const std::uint8_t* const*>(base() + kRenderer);
    if (!renderer)
        return false;
    const auto* fsr = *reinterpret_cast<const std::uint8_t* const*>(renderer + kRendererFsr);
    return fsr && *reinterpret_cast<const int*>(fsr + kFsrMethod) != 0;
}
bool fsr_on() { return buildFree ? fsrSeen : game_fsr_on(); }

// A frame end with both eye passes: whether FSR wrote an eye's image this frame; two frames in a row that say otherwise
// change the build-free answer (one stray frame, a loading screen's, doesn't).
void note_fsr(bool passes) {
    const bool written = mvr_fsr_written();
    if (!passes)
        return;
    if (knownBuild) {
        const bool game = game_fsr_on();
        char detail[96];
        std::snprintf(detail, sizeof(detail), "the layer saw FSR %s, the game's setting says %s", written ? "on" : "off",
                      game ? "on" : "off");
        fact(3, written == game, detail);
    }
    if (written == fsrSeen) {
        fsrSeenFrames = 0;
        return;
    }
    if (++fsrSeenFrames < 2)
        return;
    fsrSeenFrames = 0;
    fsrSeen = written;
    if (buildFree)
        simqvfr::logf("lmu qvfr: frame %llu: FSR %s (its compute writes into the eye's image %s): from the next frame",
                      frames, written ? "on" : "off", written ? "seen" : "gone");
}

// Pass 0 on a known build: the layer's facts against the game's (the log; nothing changes with them).
void check_facts() {
    if (!knownBuild)
        return;
    ++factFrames;
    std::uint32_t game[2], layer[2] = {offeredW.load(std::memory_order_relaxed), offeredH.load(std::memory_order_relaxed)};
    game_recommended(game);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "layer %ux%u, game %ux%u", layer[0], layer[1], game[0], game[1]);
    fact(0, layer[0] == game[0] && layer[1] == game[1], detail);
    unsigned scene[2];
    const float scale = game_render_scale();
    if (mvr_scene_size(scene[0], scene[1]) && scale > 0) {
        const unsigned want[2] = {scaled_size(game[0], scale), scaled_size(game[1], scale)};
        const float layerScale = render_scale_from_sizes(scene[0], scene[1], layer[0], layer[1]);
        std::snprintf(detail, sizeof(detail), "scene target %ux%u (scale %.4f), the game's scale %.4f makes %ux%u", scene[0],
                      scene[1], layerScale, scale, want[0], want[1]);
        fact(1, scene[0] == want[0] && scene[1] == want[1] && layerScale == scale, detail);
    }
    for (int e = 0; e < 2; ++e) {
        float mine[16];
        if (!layer_slot(e, mine))
            continue;
        const float* theirs = game_slot(e, 0);
        const int at[4] = {0, 5, 8, 9};
        float worst = 0;
        for (int i : at)
            worst = (std::max)(worst, std::fabs(mine[i] - theirs[i]));
        std::snprintf(detail, sizeof(detail),
                      "eye %d: m0 %.5f/%.5f m5 %.5f/%.5f m8 %.5f/%.5f m9 %.5f/%.5f (layer/game; largest gap %.2g)", e,
                      mine[0], theirs[0], mine[5], theirs[5], mine[8], theirs[8], mine[9], theirs[9], worst);
        fact(2, worst < 1e-4f, detail);
    }
    if (factFrames % 5400 == 0)
        log_fact_checks("so far");
}

// Pass p's part of its eye target from what it rendered of the main targets (passW from sceneW), at most the target.
void set_final_size(int p) {
    unsigned m[2];
    main_size(m);
    std::uint32_t rec[2];
    real_recommended(rec);
    extentW[p] = final_extent(sceneW[p], m[0], rec[0]);
    extentH[p] = final_extent(sceneH[p], m[1], rec[1]);
    passW[p] = final_texels(extentW[p], viewW);
    passH[p] = final_texels(extentH[p], viewH);
    if (viewW && extentW[p] > viewW)
        extentW[p] = viewW;
    if (viewH && extentH[p] > viewH)
        extentH[p] = viewH;
}

bool plain_now();  // the eyes at the game's own size meanwhile (mvr_recovery.h; below)

// ---- One eye off ([quad] eye_off, quad/eye_off.h; GameFeatures::eyeOff) ----
// The setting as each frame's pass 0 reads it, so both passes of a frame agree. That eye's pass draws no focus view,
// its scene is cut to kOffEyeSize square in the corner (mvr_size_cut: its pixels' work gone; its draws, the
// post-processing over its targets and the game's own CPU stay), DLSS leaves it out (mvr_eye_off), and the frame is
// QVFR's with the other eye's focus final alone (after_two_passes). The layer shows black in that eye (eye_off.cpp).
constexpr unsigned kOffEyeSize = 16;
int frameEyeOff = simqvfr::kEyeOffNone;
int loggedEyeOff = simqvfr::kEyeOffNone;
unsigned long long offEyePasses = 0;  // eye passes rendered off since the session began (the log)
void eye_off_frame_begin() {
    const int eye = simqvfr::eye_off();
    frameEyeOff = eye;
    mvr_eye_off(eye);
    if (eye != loggedEyeOff) {
        loggedEyeOff = eye;
        simqvfr::logf("lmu qvfr: frame %llu: %s%s (%llu eye passes rendered off so far)", frames,
                      simqvfr::eye_off_name(eye),
                      eye == simqvfr::kEyeOffNone
                          ? ""
                          : ": its pass with no focus view, its scene cut to 16x16, no DLSS on it (the headset shows it "
                            "black)",
                      offEyePasses);
    }
}
bool off_pass(int p) { return p < 2 && p == frameEyeOff; }

// At a pass's start: the game's own scale (first seen, or changed by the game: its video settings) and the size the pass's
// eye is cut to (the whole of the main targets while recovery shows the eyes alone).
void qvfr_before_pass(int p) {
    if (p >= 4)
        return;
    if (p == 0)
        check_facts();
    const float scale = buildFree ? layer_render_scale() : game_render_scale();
    if (scale > 0 && scale != gameScale) {
        if (gameScale > 0)
            simqvfr::logf("lmu qvfr: frame %llu: the game's render scale changed (%.3f -> %.3f): live scales from it",
                          frames, gameScale, scale);
        gameScale = scale;
        stretchLogged = false;
    }
    unsigned m[2];
    main_size(m);
    sceneW[p] = m[0] ? m[0] : 1u;
    sceneH[p] = m[1] ? m[1] : 1u;
    if (gameScale > 0 && m[0] && m[1] && !plain_now()) {
        float share;
        {
            std::lock_guard guard(liveMutex);
            share = liveScale[0];
        }
        std::uint32_t rec[2];
        real_recommended(rec);
        sceneW[p] = scaled_size(rec[0], gameScale * share);
        sceneH[p] = scaled_size(rec[1], gameScale * share);
    }
    if (m[0] && m[1]) {
        sceneW[p] = std::min(sceneW[p], m[0]);
        sceneH[p] = std::min(sceneH[p], m[1]);
    }
    set_final_size(p);
    if (!stretchLogged && m[0] && viewW) {
        stretchLogged = true;
        std::uint32_t rec[2];
        real_recommended(rec);
        if (m[0] != rec[0] || m[1] != rec[1])
            simqvfr::logf("lmu qvfr: frame %llu: the game's render scale is %.3f (FSR %s): its main targets are %ux%u, "
                          "stretched over the %ux%u eye targets by %s; each view is taken from the part of its eye target "
                          "it covers (pass %d: %ux%u rendered, %ux%u in the eye target)",
                          frames, gameScale, fsr_on() ? "on" : "off", m[0], m[1], viewW, viewH,
                          fsr_on() ? "FSR's upscale" : "the final blit", p, sceneW[p], sceneH[p], passW[p], passH[p]);
    }
}

// The focus views stopped coming (mvr_recovery.h): the eyes at full resolution meanwhile, a try again after a wait. No
// fallback for the session from it.
MvrRecovery recovery;
bool plain_now() { return cfg.trackerDrives && recovery.plain(); }
int recoveryLogged = 0;
unsigned long long eyesOnlyRun = 0;  // scene frames in a row without focus views (the overlay's badge)
// A frame with eye passes but no four views: the part of each eye's image its pass rendered (the top-left, at the live
// resolution), which the layer submits in place of the whole image (qvfr_frame_eye_extent).
bool eyeExtentReady = false;
unsigned eyeExtent[2][2] = {};

// ---- MVR (lmu_mvr, mvr_pass.h): passes 0-1 draw their focus views beside the eye ----

bool mvrInstalled = false;
// The eye passes' room check (mvr_before_pass). A refusal is not expected (the eyes are cut to the live size from the
// first scene frame); the first scene frame after others and a later one are logged apart, once each, and counted.
bool mvrRoomLogged[2] = {};  // [0] the first scene frame after others, [1] a later one
bool lastFrameScene = false;  // the frame before rendered the scene (its passes at the live resolution)
unsigned long long mvrRoomRefused = 0, mvrRoomRefusedLater = 0, mvrNoBox = 0;
char mvrLastRefusal[160] = {};  // the last refusal's sizes
// Two-pass frames without both focus finals, by the first reason found (after_two_passes); and those with both.
unsigned long long finalsNoBlit = 0, finalsNoBox = 0, finalsOutside = 0, finalsNoViews = 0, finalsBoth = 0;
// R4a (docs/recon-lmu.md "R4 plan"): each frame runs the game's two eye passes only, its focus views the focus finals
// beside the eyes (mvr_focus_final); the atlas takes them from there.
bool frameFinalsOk = false;          // this frame's focus finals both made (after_two_passes)
unsigned finalAt[2][2] = {};  // each focus final's top-left in its eye's texture
bool finalsMissingLogged = false;
unsigned long long twoPassFrames = 0;

// MVR's context hooks on `device` (at the first pass, or with the pass tracker at the session's first frame end, so menus
// are seen too). False: MVR off for the session.
ID3D11Device* sessionDevice = nullptr;  // qvfr_frame_device (not referenced: the session keeps it)
void pass_begin(int p);
void pass_end(int p);
void panel_draw(ID3D11DeviceContext* context, bool after, unsigned count, unsigned start);
bool install_mvr(ID3D11Device* device) {
    mvr_focus_replay(cfg.focusReplay);  // before the install: the draws hooked for it only
    mvr_ui_mips(cfg.uiMips);
    mvr_glass_depth(cfg.glassDepth);  // before the install: its state hooks only when on
    // Texture sharpening's pixel shader hook only when it's on at launch (no cost otherwise; its three settings are live
    // from then on) and where DLSS can run: a build with NGX, an NVIDIA GPU that draws two views, the passes the
    // tracker drives (DLSS's only way in).
    mvr_mip_bias_hooks(cfg.textureSharpening && simqvfr::ngx::built_in() && simqvfr::nvapi::two_views_gpu() != nullptr &&
                       cfg.trackerDrives);
    // Other cars' motion: the player's where DLSS can run (as texture sharpening's), the diagnostics' as they say; their
    // hooks only when on (the tracking before the slots: they need it).
    const bool cars = cfg.carTrack && (cfg.carForced || (simqvfr::ngx::built_in() &&
                                                         simqvfr::nvapi::two_views_gpu() != nullptr && cfg.trackerDrives));
    mvr_car_track(cars);
    mvr_car_slots(cars ? cfg.carSlots : 0);
    mvr_car_slot_images(cars && cfg.carForced);  // a freeze each: never the player's
    carsOn = cars && cfg.carSlots >= 2;  // the A/B measures what players get (DLSS taking the motion)
    // FSR seen at the d3d11 level: build-free runs take it from there, a known build compares it with the game's setting.
    mvr_fsr_watch(cfg.trackerDrives);
    // DLSS's camera from the per-draw buffer (view_pair.h): where it is the source (build-free), and where the cars'
    // hooks are in anyway (a known build's comparison with the renderer's, at no cost).
    mvr_view_track(cfg.trackerDrives && (buildFree || cars));
    mvr_viewport_reads(cfg.viewportReads);  // the game's viewport reads answered with its own (lmu_viewport_reads)
    mvrInstalled = device && mvr_pass_install(device);
    if (!mvrInstalled) {
        cfg.mvr = false;
        return false;
    }
    if (cfg.passTracker) {
        mvr_track_passes(true);
        mvr_pass_trace(cfg.passTrace);
        mvr_post_census(cfg.postCensus);
        if (cfg.trackerDrives) {
            mvr_track_drive(&pass_begin, &pass_end);
            mvr_mask_stale(&maskStale);  // R6c: the lens mask's draw dropped while stale
            if (qvfrMode)
                mvr_panel_draws(&panel_draw);  // R6b: the menus at native from the VR UI panel's draws
        }
        simqvfr::log_line(cfg.trackerDrives
                              ? "lmu mvr: pass tracker drives the eye passes (lmu_pass_tracker=2): their bounds from the d3d11 "
                                "calls alone; the game's scene render, eye loop, post-effect time, scene skip, glare and "
                                "driver tag hooks not installed"
                              : "lmu mvr: pass tracker on (lmu_pass_tracker=1, observe only): the eye passes as the d3d11 "
                                "calls show them, compared with the game's in the log");
    }
    if (cfg.focusReplay)
        simqvfr::log_line("lmu mvr: focus post on (lmu_focus_replay=1): after each eye pass's YEBIS copy and composite, "
                          "the same for its focus view, with the eye's glare and exposure; its final beside the eye");
    if (cfg.focusFinal)
        simqvfr::log_line("lmu mvr: focus finals on: the game's two eye passes, the focus views from beside the eyes");
    return true;
}

// Before eye pass p: its focus view's size, this frame's remap (frozen at pass 0) and the room check.
void mvr_before_pass(int p) {
    if (!mvrInstalled)  // (the tracker's passes come only once its hooks are in: qvfr_frame_frame_end)
        return;
    if (p == 0) {
        mvr_frame_mode(0);
        {
            if (cfg.backendAb > 0) {  // NVIDIA's or instancing this frame (between passes)
                if (!backendAbLogged) {
                    backendAbLogged = true;
                    backendAbOk = mvr_probe_backend() == MvrBackend::nvapi && mvr_probe_instanced();
                    if (backendAbOk)
                        simqvfr::logf("lmu mvr: backend A/B: %d frames with NVIDIA's multi-view rendering, then %d "
                                      "with instancing, in turn (lmu_mvr_backend_ab)",
                                      cfg.backendAb, cfg.backendAb);
                    else
                        simqvfr::logf("lmu mvr: lmu_mvr_backend_ab=%d ignored: it needs NVIDIA's multi-view rendering "
                                      "and the instanced twins (an RTX 20 series or newer, without "
                                      "multi_view_software=1)",
                                      cfg.backendAb);
                }
                if (backendAbOk && !mvr_broken()) {
                    backendSide = static_cast<int>((sceneFrames / static_cast<unsigned long long>(cfg.backendAb)) % 2);
                    if (mvr_instanced() != (backendSide == 1))
                        mvr_use_instanced(backendSide == 1, true);
                    mvr_frame_mode(backendSide);
                }
            }
            if (car_ab_on()) {  // the other cars' motion this frame or not (between frames)
                if (!carAbLogged) {
                    carAbLogged = true;
                    simqvfr::logf("lmu mvr: other cars' motion A/B: %d frames with it, then %d without, in turn "
                                  "(lmu_car_vectors_ab)",
                                  cfg.carAb, cfg.carAb);
                }
                carSide = static_cast<int>((sceneFrames / static_cast<unsigned long long>(cfg.carAb)) % 2);
                mvr_car_active(carSide == 0);
                mvr_frame_mode(carSide);
            } else if (carsOn) {  // DLSS or the cars' motion turned off while the game runs: no tracking for nothing
                const bool pause = !carsWanted.load(std::memory_order_relaxed);
                if (pause != carsPaused) {
                    carsPaused = pause;
                    mvr_car_active(!pause);
                    simqvfr::log_line(pause ? "lmu mvr: other cars' motion paused (DLSS or its car motion turned off; "
                                              "the hooks stay in until the session ends)"
                                            : "lmu mvr: other cars' motion resumed");
                }
            }
            if (cfg.maskAb > 0) {  // the lens mask this frame or not (between frames)
                if (!maskAbLogged) {
                    maskAbLogged = true;
                    simqvfr::logf("lmu mvr: lens mask A/B: %d frames with the game's mask draw, then %d without, in "
                                  "turn (lmu_mask_ab)",
                                  cfg.maskAb, cfg.maskAb);
                }
                maskSide = static_cast<int>((sceneFrames / static_cast<unsigned long long>(cfg.maskAb)) % 2);
                mvr_mask_ab_drop(maskSide == 1);
                mvr_frame_mode(maskSide);
            }
            {  // the focus mask as last set (live)
                std::lock_guard guard(liveMutex);
                mvr_focus_mask(focusMaskOn, focusMaskArea, focusMaskMargin);
            }
        }
        freeze_focus();
    }
    MvrPassView v;
    bool draw = false;
    if (off_pass(p)) {
        // The eye that's off: no focus view (nothing of it is shown).
    } else if (p < 2 && frozen[p] && gameScale > 0 && !recovery.plain()) {  // (plain: no focus view asked for)
        const ClipRemap& r = frozenRemap[p];
        v.eye = p;
        v.remap[0] = static_cast<float>(r.a);
        v.remap[1] = static_cast<float>(r.b);
        v.remap[2] = static_cast<float>(r.c);
        v.remap[3] = static_cast<float>(r.d);
        v.eyeW = sceneW[p];  // its corner of the main targets, where the focus view goes beside it
        v.eyeH = sceneH[p];
        v.finalEyeW = passW[p];  // that corner in its final (stretched by the game's blit below render scale 1)
        v.finalEyeH = passH[p];
        v.box = frozenBox[p];
        float shareW, shareH;
        {
            std::lock_guard guard(liveMutex);
            shareW = cfg.perAxis ? liveAxes[1][0] : liveScale[1];
            shareH = cfg.perAxis ? liveAxes[1][1] : liveScale[1];
        }
        std::uint32_t recommended[2];
        real_recommended(recommended);
        v.focusW = scaled_size(recommended[0], gameScale * shareW);
        v.focusH = scaled_size(recommended[1], gameScale * shareH);
        // DLSS super resolution on the focus view (engine/dlss_views.h dlss_focus_scale): the scene draws it smaller in
        // the same corner, DLSS upscales it into its region before the post; 1 (its size) unless that's on and working.
        const double srScale = simqvfr::dlss_focus_scale();
        v.focusRenderW = simqvfr::dlss::render_size(v.focusW, srScale);
        v.focusRenderH = simqvfr::dlss::render_size(v.focusH, srScale);
        // DLAA (engine/dlss_views.h): the focus view's own jitter, through its remap (in the pixels it's drawn at); the
        // eye's is in its projection. Both are what the overlays after the post are drawn moved back by
        // (MvrPassView::overlayShift).
        const float remapB = v.remap[1], remapD = v.remap[3];
        simqvfr::dlss_focus_jitter(p, v.focusRenderW, v.focusRenderH, v.remap[1], v.remap[3]);
        v.overlayShift[1][0] = v.remap[1] - remapB;
        v.overlayShift[1][1] = v.remap[3] - remapD;
        simqvfr::dlss_eye_clip_shift(p, v.overlayShift[0][0], v.overlayShift[0][1]);
        simqvfr::dlss_mip_bias(p, v.mipBias[1], v.mipBias[0]);
        if ((v.mipBias[0] != 0 || v.mipBias[1] != 0) && !mvr_mip_bias_ready()) {  // turned on while the game runs
            static bool logged = false;
            if (!logged) {
                logged = true;
                simqvfr::log_line("lmu mvr: texture sharpening turned on, but it was off at launch (its hook and shader "
                                  "twins weren't made): it starts at the next launch");
            }
        }
        // Side by side in the main targets (the scene's), and the focus final beside the eye's corner of its final.
        unsigned room[2];
        main_size(room);
        if (!room[0] || !room[1]) {
            room[0] = viewW;
            room[1] = viewH;
        }
        draw = v.eyeW && v.focusW && (!room[0] || (v.eyeW + v.focusW <= room[0] && v.focusH <= room[1])) &&
               (!viewW || (v.finalEyeW + v.focusW <= viewW && v.focusH <= viewH));
        if (!draw) {
            const int later = lastFrameScene ? 1 : 0;
            ++mvrRoomRefused;
            mvrRoomRefusedLater += later;
            std::snprintf(mvrLastRefusal, sizeof(mvrLastRefusal),
                          "frame %llu pass %d: eye %ux%u, focus %ux%u, targets %ux%u, the eye %ux%u of its %ux%u final",
                          frames, p, v.eyeW, v.eyeH, v.focusW, v.focusH, room[0], room[1], v.finalEyeW, v.finalEyeH, viewW,
                          viewH);
            if (!mvrRoomLogged[later]) {
                mvrRoomLogged[later] = true;
                simqvfr::logf("lmu mvr: frame %llu: eye %ux%u and focus %ux%u don't fit side by side in %ux%u targets (the "
                              "eye %ux%u of its %ux%u final)%s: %s",
                              frames, v.eyeW, v.eyeH, v.focusW, v.focusH, room[0], room[1], v.finalEyeW, v.finalEyeH, viewW,
                              viewH,
                              later ? ", on a frame after a scene frame (its passes at the live resolution: not expected)"
                                    : ", the first scene frame after menus or loading (its passes at the game's own size "
                                      "until the live resolution applies: expected)",
                              "no focus views this frame");
            }
        }
    } else {
        ++mvrNoBox;  // no focus box this frame (its projection, or the game's scale not known yet)
    }
    mvr_pass_begin(p, draw ? &v : nullptr, false);  // (no gate samples)
}

// The view `src` (its top-left w x h) into the atlas at (x, y), or (atlas_out) the atlas's at (x, y) into `dst`.
void atlas_in(ID3D11Texture2D* src, unsigned x, unsigned y, unsigned w, unsigned h) {
    ID3D11Device* device = nullptr;
    atlas->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D11_BOX box{0, 0, 0, std::min(w, viewW), std::min(h, viewH), 1};
    context->CopySubresourceRegion(atlas, 0, x, y, 0, src, 0, &box);
    context->Release();
    device->Release();
}

// `src`'s w x h at (sx, sy) into the atlas at (x, y) (R4a: a focus final beside its eye).
void atlas_in_from(ID3D11Texture2D* src, unsigned sx, unsigned sy, unsigned x, unsigned y, unsigned w, unsigned h) {
    ID3D11Device* device = nullptr;
    atlas->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D11_BOX box{sx, sy, 0, sx + std::min(w, viewW), sy + std::min(h, viewH), 1};
    context->CopySubresourceRegion(atlas, 0, x, y, 0, src, 0, &box);
    context->Release();
    device->Release();
}

// The frame's record for the layer (adapter.h Views4Frame: tangents left, right, up, down; the focus views off-centre,
// unturned). Under liveMutex.
simqvfr::Views4Frame latestRecord{};
bool haveRecord = false;

void store_record() {
    simqvfr::Views4Frame f{};
    for (int e = 0; e < 2; ++e) {
        const FocusBox& b = boxes[e];
        const float outer[4] = {b.eyeTan[0], b.eyeTan[1], b.eyeTan[3], b.eyeTan[2]};
        const float focus[4] = {b.focusTan[0], b.focusTan[1], b.focusTan[3], b.focusTan[2]};
        std::memcpy(f.outerTan[e], outer, sizeof(outer));
        std::memcpy(f.focusTan[e], focus, sizeof(focus));
        f.turn[e] = {0, 0, 0, 1};
    }
    // The sizes rendered, as views4_live_one_size lays them out (each view half its layer at most: 0.5 = all of it): each
    // view's exact part of its eye target (extentW: the stretched one below the game's render scale 1).
    f.live = true;
    for (int v = 0; v < 4; ++v) {
        f.width[v] = f.renderWidth[v] = viewW ? 0.5 * extentW[v] / viewW : 0.5;
        f.height[v] = f.renderHeight[v] = viewH ? 0.5 * extentH[v] / viewH : 0.5;
    }
    std::lock_guard guard(liveMutex);
    latestRecord = f;
    haveRecord = true;
}

// R4a, after pass 1 of a two-pass frame: the eye loop ends here; the eyes stay in their targets with the focus finals
// beside them (the VR UI is doubled onto those after the loop). The frame's record and sizes from the focus finals;
// the atlas takes them at xrEndFrame (qvfr_frame_atlas). No focus finals: no four views this frame.
void after_two_passes() {
    atlasReady = false;  // this pass's finals decide (pass 0 cleared it already, unless the tracker saw pass 1 again)
    ID3D11Texture2D* e0 = game_eye_image(0);
    ID3D11Texture2D* e1 = game_eye_image(1);
    // The multi-view path, once: the copies' eye images are the targets the passes' final blits drew into.
    if (cfg.trackerDrives && !eyeImagesChecked && e0 && e1 && mvr_eye_final(0) && mvr_eye_final(1)) {
        eyeImagesChecked = true;
        const bool same = mvr_eye_final(0) == e0 && mvr_eye_final(1) == e1;
        simqvfr::logf("lmu qvfr: frame %llu: the eye images from the submit's copies %s the final blits' targets "
                      "(passes 0 and 1)",
                      frames, same ? "are" : "are NOT");
    }
    if (e0 && e1 && ensure_atlas(e0) && is_view(e1)) {
        for (int q = 0; q < 2; ++q)  // the first frame's sizes were taken before the atlas knew the views
            set_final_size(q);
        bool ok = true;
        for (int e = 0; e < 2 && ok; ++e) {
            if (off_pass(e)) {
                // The eye that's off drew none: the frame is QVFR's with the other's alone. Its focus view's record
                // stays valid (its box, a few pixels from the eye's corner), so the composition runs as for any frame;
                // the layer shows the eye black whatever it holds.
                finalAt[e][0] = finalAt[e][1] = 0;
                passW[2 + e] = passH[2 + e] = std::min(kOffEyeSize, std::min(viewW, viewH));
                extentW[2 + e] = passW[2 + e];
                extentH[2 + e] = passH[2 + e];
                if (frozen[e])
                    boxes[e] = frozenBox[e];
                continue;
            }
            unsigned x = 0, y = 0, w = 0, h = 0;
            const bool final = mvr_focus_final(e, x, y, w, h);
            ok = final && frozen[e] && x + w <= viewW && y + h <= viewH && w && h;
            if (!ok)  // the fallback's summary: the first reason found
                ++(!final ? finalsNoBlit : !frozen[e] ? finalsNoBox : finalsOutside);
            if (ok) {
                finalAt[e][0] = x;
                finalAt[e][1] = y;
                passW[2 + e] = w;  // the focus final: the focus image 1:1 beside the eye
                passH[2 + e] = h;
                extentW[2 + e] = w;
                extentH[2 + e] = h;
                boxes[e] = frozenBox[e];
            }
        }
        frameFinalsOk = ok;
        if (ok) {
            ++finalsBoth;
            atlasReady = true;
            store_record();
            if (gpuSpans.ready(e0))
                gpuSpans.begin_frame();
        } else {
            if (!finalsMissingLogged) {
                finalsMissingLogged = true;
                simqvfr::logf("lmu mvr: frame %llu: two passes without both focus finals: no four views this frame (the eyes "
                              "as they are)",
                              frames);
            }
        }
    } else {
        ++finalsNoViews;  // the eye images not found, or not the views' size
    }
    if (e0)
        e0->Release();
    if (e1)
        e1->Release();
}

void qvfr_after_pass(int p) {
    if (p == 1)
        after_two_passes();
}

// The work around one eye pass, at the bounds R4b's pass tracker finds at the d3d11 level (mvr_pass.h mvr_track_drive):
// pass_begin before the pass's draws, pass_end after them, on the render thread.
double passWall0 = 0;
unsigned long long passCpu0 = 0;
int trackedEyeLogged = 0;                    // tracked passes that rendered another eye: the first 5 logged
unsigned long long trackedEyeMismatches = 0;  // and counted

void pass_begin(int p) {
    renderThread = GetCurrentThreadId();
    ++passesThisFrame;
    if (p == 0) {
        atlasReady = menuReady = false;  // a new frame: its own four views, or none
        frameFinalsOk = false;
        eye_off_frame_begin();
    }
    unsigned long long hook0 = thread_cycles();
    qvfr_before_pass(p);  // live resolution: the size this pass renders at
    if (cfg.mvr && p < 2) {
        unsigned m[2];
        main_size(m);
        if (off_pass(p)) {  // the eye that's off: a few pixels, whatever the live size or a recovery
            ++offEyePasses;
            mvr_size_cut(true, m[0], m[1], std::min(kOffEyeSize, m[0]), std::min(kOffEyeSize, m[1]));
        } else {
            mvr_size_cut(!plain_now(), m[0], m[1], sceneW[p], sceneH[p]);
        }
        mvr_before_pass(p);
    }
    if (p == 0)  // the frame capture hotkey as this frame's first pass sees it (capture_views)
        frameCaptureArm = simqvfr::d3d11_capture_armed();
    passWall0 = now_ms();
    passCpu0 = thread_cycles();
    costWindow.hookCycles += static_cast<double>(passCpu0 - hook0);
}

void pass_end(int p) {
    const double wall0 = passWall0;
    const unsigned long long cpu0 = passCpu0;
    if (cfg.mvr && p < 2)
        mvr_pass_end(p);
    const unsigned long long cpu1 = thread_cycles();
    if (p < 4) {  // what the game's own pass cost (qvfr_frame_frame_end logs the averages)
        costWindow.passWallMs[p] += now_ms() - wall0;
        costWindow.passCpuCycles[p] += static_cast<double>(cpu1 - cpu0);
        if (cfg.mvr && ab_run())
            ab.cpu[ab_side()][p] += static_cast<double>(cpu1 - cpu0);
    }
    if (p < 4) {
        // The eye the game rendered: its view params' on a known build; build-free, the passes' order (left first, as
        // the game's eye loop runs them).
        void* renderer = buildFree ? nullptr : *reinterpret_cast<void**>(base() + kRenderer);
        eyeOfPass[p] = buildFree ? p
                       : renderer ? *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(renderer) + kRendererViewEye)
                                  : -1;
        if (p < 2 && eyeOfPass[p] != p) {
            ++trackedEyeMismatches;
            if (trackedEyeLogged < 5) {
                ++trackedEyeLogged;
                simqvfr::logf("lmu mvr: pass tracker: frame %llu: the tracked pass %d rendered the game's eye %d (%llu "
                              "such passes so far)",
                              frames, p, eyeOfPass[p], trackedEyeMismatches);
            }
        }
        qvfr_after_pass(p);
        costWindow.hookCycles += static_cast<double>(thread_cycles() - cpu1);
    }
}

const char* tag() { return "lmu qvfr"; }

// ---- QVFR: the four views at the frame capture hotkey ----
// [experiment] d3d11_capture_hotkey=1: each press saves the swapchains' images (probe/d3d11_capture.h); the next composed
// frame's four views are saved too, each as far as it rendered, and the log gives each focus view's box in its eye:
// what tools/halo_profile.py and tools/focus_compare.py measure.
void capture_views() {
    const std::uint64_t arm = frameCaptureArm;  // as this frame's pass 0 saw it (pass_begin)
    if (arm == seenCaptureArm)
        return;
    seenCaptureArm = arm;
    if (arm == 0 || arm == 0xFFFFFFFFull)  // no session, or the hotkey not pressed yet
        return;
    std::vector<std::uint8_t> px;
    unsigned w = 0, h = 0;
    if (!simqvfr::d3d11_read_bgra8(atlas, px, w, h) || w < 2 * viewW || h < 2 * viewH) {
        simqvfr::logf("lmu qvfr: frame capture: frame %llu's views couldn't be read back", frames);
        return;
    }
    const std::wstring dir = simqvfr::captures_directory();
    const wchar_t* names[4] = {L"eye0", L"eye1", L"focus0", L"focus1"};
    std::wstring paths[4];
    for (int v = 0; v < 4; ++v) {
        const unsigned x0 = (v % 2) * viewW, y0 = (v / 2) * viewH;
        const unsigned cw = std::clamp(passW[v], 1u, viewW), ch = std::clamp(passH[v], 1u, viewH);
        std::vector<std::uint8_t> view(static_cast<std::size_t>(cw) * ch * 4);
        for (unsigned y = 0; y < ch; ++y)
            std::memcpy(view.data() + static_cast<std::size_t>(y) * cw * 4,
                        px.data() + (static_cast<std::size_t>(y0 + y) * w + x0) * 4, static_cast<std::size_t>(cw) * 4);
        wchar_t name[64];
        swprintf_s(name, L"lmu-qvfr-frame%llu-%ls.png", frames, names[v]);
        paths[v] = (std::filesystem::path(dir) / name).wstring();
        if (!simqvfr::write_png_bgra(paths[v], view, cw, ch))
            simqvfr::logf("lmu qvfr: frame capture: %ls not written", paths[v].c_str());
    }
    for (int e = 0; e < 2; ++e) {
        const double* r = boxes[e].rect;
        simqvfr::logf("lmu qvfr: frame capture %llu: eye %d with focus view %d: python "
                      "tools/halo_profile.py --eye-image \"%ls\" --focus-image \"%ls\" --box %.5f %.5f %.5f %.5f",
                      frames, e, e, paths[e].c_str(), paths[e + 2].c_str(), r[0], r[1], r[2], r[3]);
    }
}

}  // namespace qvfr_frame_detail

using namespace qvfr_frame_detail;

// R5: the multi-view path (the tracker driving the game's two passes) gives way to the four passes without MVR, for
// the rest of the session: at xrCreateSession when the device can't do it, or at a frame end (render thread, between
// frames) when MVR fails while running. Logged once with `why`.
int badgeLogged = 0;
bool losingForTest = false;  // lmu_mvr_lose_at's window is open
std::atomic<int> multiViewNow{0};  // qvfr_frame_multi_view's (a QvfrMultiView), as of the last frame end

// What the multi-view path did before it stopped, step by step, so a log shows which one the focus views didn't get past:
// the eye passes' room check and focus box (mvr_before_pass), drawing the focus view, the focus post's copy, composite
// and final blit (mvr_pass), and the frame's two focus finals (after_two_passes). Session totals.
void log_multi_view_summary() {
    const MvrPassStats s = mvr_pass_stats();
    const MvrProbeStats probe = mvr_probe_stats();
    const auto u = [](unsigned long long v) { return v; };
    simqvfr::logf("lmu mvr: before it stopped: %llu two-pass frames, %llu with both focus finals; without them: %llu no "
                  "final blit beside an eye, %llu no focus box, %llu a final outside the image, %llu the eye images not "
                  "the views; eye passes: %llu refused for room (%llu on a frame after a scene frame)%s%s, %llu without a "
                  "focus box, %llu drew their focus view (%llu twin binds, %llu stereo switches refused); focus post: %llu "
                  "copies, %llu composites (%llu by their bindings), %llu final blits, %llu steps failed, %llu passes "
                  "without the composite after the copy, %llu draws into the resolve's target by another shader; YEBIS's "
                  "copy, composite and final blit pixel shaders marked %llu, %llu and %llu times",
                  twoPassFrames, finalsBoth, finalsNoBlit, finalsNoBox, finalsOutside, finalsNoViews, mvrRoomRefused,
                  mvrRoomRefusedLater, mvrLastRefusal[0] ? "; the last: " : "", mvrLastRefusal, mvrNoBox,
                  u(s.passesWithFocus), u(s.twinBinds), u(s.stereoErrors), u(s.focusCopies), u(s.focusComposites),
                  u(s.focusCompositesByBindings), u(s.focusBlits), u(s.focusFailures), u(s.focusNoComposite),
                  u(s.focusUnknownComposites), u(probe.yebisCopies), u(probe.yebisComposites), u(probe.finalBlits));
}

// Multi-view rendering can't run at all (the driver refused it and instancing couldn't take over, or the game's device
// can do neither): the eyes alone at full resolution, without focus views, for the rest of the session. Nothing of the
// game's is hooked in its place (the four passes went on 2026-11-12). Logged once with `why`.
void stop_multi_view(const char* why) {
    if (recovery.stopped)
        return;
    if (twoPassFrames)  // it ran (not the session start's stop, where the device can't)
        log_multi_view_summary();
    recovery.stop();
    mvr_focus_replay(false);
    simqvfr::logf("lmu mvr: frame %llu: multi-view rendering stopped (%s): the eyes alone at full resolution, without "
                  "focus views, for the rest of the session",
                  frames, why);
}

bool qvfr_frame_start(const QvfrFrameRun& run) {
    if (installed)
        return true;
    cfg = run;
    qvfrMode = run.qvfr;
    menuNative = run.qvfr && run.menuNative && run.layerWidth > 0 && run.layerHeight > 0;
    layerShare[0] = run.layerWidth;
    layerShare[1] = run.layerHeight;
    qvfr_frame_set_box(run.fraction, run.fractionY);
    // Any build: on one abi.h's addresses are from the game's facts are read there (and compared with the layer's), on
    // any other they come from what the layer sees (buildFree, above).
    knownBuild = exe_known_build();
    buildFree = !knownBuild || run.buildFree;
    if (!knownBuild)
        simqvfr::logf("%s: a game build Sim-QVFR wasn't made for (sha256 %s): the eye size, render scale, FSR and eye "
                      "projections from what OpenXR and Direct3D show, DLSS's camera from the draws' constant buffers, "
                      "nothing read in the game",
                      tag(), buildSha.c_str());
    else if (buildFree)
        simqvfr::logf("%s: lmu_build_free=1: the game's facts from what OpenXR and Direct3D show, as on a build Sim-QVFR "
                      "wasn't made for (compared with the game's in the log)",
                      tag());
    else
        simqvfr::logf("%s: a build Sim-QVFR was made for: the game's facts read in it, compared with what OpenXR and "
                      "Direct3D show (the log's build-free lines)",
                      tag());
    // The game's own two eye passes, found at the d3d11 level, and the lens mask and the VR UI from their d3d11 draws:
    // no game code hooked.
    const bool driven = cfg.trackerDrives;
    if (!simqvfr::minhook_ready(tag()))
        return false;
    if (driven) {
        installed = true;
        mvr_time_hooks(cfg.mvrTiming);
        if (qvfrMode) {
            std::lock_guard guard(liveMutex);
            simqvfr::logf("lmu qvfr: on with multi-view rendering and the pass tracker: the game's own two eye passes draw "
                          "the focus views beside the eyes, a %.0f%% x %.0f%% box of each eye's tangent span on the gaze; "
                          "game code hooked: none (the lens mask and the VR UI handled at their d3d11 draws)",
                          focusFraction * 100, (focusFractionY > 0 ? focusFractionY : focusFraction) * 100);
        }
        return true;
    }
    simqvfr::logf("%s: LMU's QVFR runs on multi-view rendering only (the four passes went on 2026-11-12), and this run "
                  "has no room for it (the layer's layout): not installed",
                  tag());
    return false;
}

void qvfr_frame_device(ID3D11Device* device) {
    sessionDevice = device;
    recovery.new_session();  // a graphics setting may have changed: no waiting for the next try
    // R5: the device decides; the game is still in its menus, the eye loop not yet run.
    if (installed && cfg.trackerDrives && !mvr_probe_usable())
        stop_multi_view("the game's device can't draw two views a pass (no NVIDIA multi-view rendering, and no vertex "
                        "shader picking the viewport), or multi-view's shader probe is off");
    else if (installed && cfg.trackerDrives)
        simqvfr::logf("lmu qvfr: multi-view rendering by %s%s", mvr_backend_name(mvr_probe_backend()),
                      mvr_probe_backend() == MvrBackend::nvapi && mvr_probe_instanced()
                          ? "; instancing takes over if the driver ever refuses it"
                          : "");
}

void qvfr_frame_frame_end() {
    if (!installed)
        return;
    ++frames;
    const bool offThread = off_render_thread();
    if (offThread && !frameEndThreadLogged) {
        frameEndThreadLogged = true;
        simqvfr::log_line("lmu qvfr: xrEndFrame isn't on the thread that rendered the views: the frame end's work on the "
                          "game's context waits for a frame end on it (logged once)");
    }
    if (cfg.mvr && cfg.passTracker && !mvrInstalled && sessionDevice && !offThread)  // the tracker sees the menus too
        if (!install_mvr(sessionDevice))
            stop_multi_view("multi-view's context hooks couldn't be installed");
    {
        simqvfr::FramePartTimer part(simqvfr::FramePart::dlssEnd);
        simqvfr::dlss_frame_end(frames);
    }
    if (!offThread)
        fixation_frame_end();  // the convergence's depth read, back
    if (cfg.mvr) {
        if (!offThread) {  // (skipped: the next frame end on the render thread ends the tracker's frame)
            simqvfr::FramePartTimer part(simqvfr::FramePart::mvrEnd);
            mvr_frame_end(frames);
        }
        frozen[0] = frozen[1] = false;
    }
    const int passes = passesThisFrame;
    passesThisFrame = 0;
    if (passes >= 2)
        ++sceneFrames;
    lastFrameScene = passes >= 2;  // the next frame's room check: its passes at the live resolution
    if (passes >= 2 && twoPassFrames++ == 0)
        simqvfr::logf("%s: frame %llu: first two-pass frame (focus finals), eyes rendered %d %d", tag(), frames, eyeOfPass[0],
                      eyeOfPass[1]);
    // R5: the multi-view path's health, between frames on the render thread. A driver that refuses multi-view rendering
    // stops it for the session (stop_multi_view: the eyes alone). Focus views that stop coming do not: after
    // half a second the eyes go to full resolution, and it tries again by itself (mvr_recovery.h).
    if (cfg.trackerDrives) {
        // FSR on: each eye's post-processing ends in FSR's upscale (compute) instead of the final blit; the focus
        // composite goes beside the eye before it, so FSR carries it into the eye's image (mvr_fsr).
        if (!offThread)
            note_fsr(passes >= 2);  // build-free: FSR as this frame's compute writes showed it
        mvr_fsr(fsr_on(), eyeImage[0], eyeImage[1]);
        const char* why = mvr_broken()           ? "the driver refused NVIDIA's multi-view rendering (and instancing "
                                                   "couldn't take over)"
                                                 : nullptr;
        if (cfg.loseAt > 0 && cfg.focusReplay && !why) {  // the recovery's test: the focus post off for a while
            const unsigned long long from = static_cast<unsigned long long>(cfg.loseAt);
            const bool lose = sceneFrames >= from && sceneFrames < from + 1800;
            if (lose != losingForTest) {
                losingForTest = lose;
                mvr_focus_replay(!lose);
                if (cfg.loseShape)  // as a pipeline that lacks a step: no try is made meanwhile
                    mvr_shape_force_bad(lose);
                simqvfr::logf("lmu mvr: frame %llu (scene frame %llu): lmu_mvr_lose_at: the focus post %s", frames, sceneFrames,
                              lose ? "off for 1800 scene frames (no focus views: the recovery's test)" : "on again");
            }
        }
        if (why) {
            stop_multi_view(why);
        } else {
            const double waited = recovery.waitMs;
            // While the eyes render alone: what each eye pass's post-processing lacks of the steps the focus views are
            // made at, now and then (the reason no try is being made).
            if (recovery.plain() && passes >= 2 && !mvr_shape_ready() && frames % 900 == 0)
                simqvfr::logf("lmu mvr: frame %llu: the eyes alone, no try: eye 0's pass has %s, eye 1's %s", frames,
                              mvr_shape_missing(0), mvr_shape_missing(1));
            switch (recovery.frame(passes, frameFinalsOk, now_ms(), mvr_shape_ready())) {
            case MvrRecovery::Event::wentPlain:
                if (recovery.timesPlain == 1 && twoPassFrames)
                    log_multi_view_summary();  // what the focus views didn't get past, once
                if (recoveryLogged++ < 12 || recovery.timesPlain % 20 == 0)
                    simqvfr::logf("lmu mvr: frame %llu: no focus views%s: the eyes at full resolution, without foveation, from "
                                  "the next frame; trying the focus views again in %.0f s (time %llu; MSAA %s)",
                                  frames, waited > 0 ? " in the try" : " in 45 frames in a row", recovery.waitMs / 1000.0,
                                  recovery.timesPlain, mvr_single_sample_passes() ? "off" : "on");
                break;
            case MvrRecovery::Event::recovered:
                simqvfr::logf("lmu mvr: frame %llu: the focus views again (after %llu times at full resolution)", frames,
                              recovery.timesPlain);
                break;
            default: break;
            }
        }
    }
    // A frame with eye passes but no four views: what the layer submits of each eye's image (its rendered part).
    eyeExtentReady = false;
    if (cfg.trackerDrives && qvfrMode && passes >= 1 && !atlasReady && viewW && viewH) {
        for (int e = 0; e < 2; ++e) {
            eyeExtent[e][0] = passW[e] ? passW[e] : passW[1 - e];
            eyeExtent[e][1] = passH[e] ? passH[e] : passH[1 - e];
        }
        eyeExtentReady = eyeExtent[0][0] && eyeExtent[0][1] && eyeExtent[1][0] && eyeExtent[1][1] &&
                         (eyeExtent[0][0] < viewW || eyeExtent[0][1] < viewH || eyeExtent[1][0] < viewW || eyeExtent[1][1] < viewH);
    }
    // The overlay's badge: the way this frame's focus views were drawn (the backend as it stands: NVIDIA's until
    // instancing takes over).
    using simqvfr::games::QvfrMultiView;
    QvfrMultiView way = QvfrMultiView::none;
    if (cfg.trackerDrives && cfg.mvr) {
        // The backend's name only while focus views are being made: scene frames without them (plain, or no eye pass
        // found at all, by the frame's draw count) say so.
        const bool scene = passes >= 1 || mvr_track_stats().lastFrameDraws >= 1000;
        if (scene)
            eyesOnlyRun = frameFinalsOk ? 0 : eyesOnlyRun + 1;
        way = recovery.plain() || eyesOnlyRun >= MvrRecovery::kNoFinalsFrames ? QvfrMultiView::eyesOnly
              : mvr_probe_backend() == MvrBackend::nvapi && !mvr_instanced() ? QvfrMultiView::nvidia
                                                                              : QvfrMultiView::software;
    }
    if (static_cast<int>(way) != multiViewNow.load(std::memory_order_relaxed) && badgeLogged++ < 40) {
        static const char* const kNames[] = {"none", "MVR", "SMVR", "Fallback", "Eyes only"};
        simqvfr::logf("lmu mvr: frame %llu: the overlay's badge: %s (last frame %d passes, focus views %s)", frames,
                      kNames[static_cast<int>(way)], passes, frameFinalsOk ? "made" : "not made");
    }
    multiViewNow.store(static_cast<int>(way), std::memory_order_relaxed);

    const int fullPasses = 2;  // the game's own two eye passes
    {
        if (passes < fullPasses) {
            atlasReady = false;  // no four views this frame (menus, loading): the layer submits the eyes as they are
            seenCaptureArm = simqvfr::d3d11_capture_armed();  // a frame capture now has no views to save (not later)
        }
        menu_finish();  // before the layer takes the atlas (views4_submit, after this)
        maskStaleThisFrame += mvr_mask_dropped();
        const bool stale = maskStale.load(std::memory_order_relaxed);
        if (stale != maskStaleLogged) {
            maskStaleLogged = stale;
            if (stale)
                simqvfr::logf("lmu qvfr: frame %llu: the located FOV no longer matches the lens mask's (a live FOV crop "
                              "change): the mask is off (%d draws skipped this frame)",
                              frames, maskStaleThisFrame);
            else
                simqvfr::logf("lmu qvfr: frame %llu: the located FOV matches the lens mask's again: the mask is on", frames);
        }
        maskStaleThisFrame = 0;
        // Every 900 frames while the scene runs: the sizes the passes rendered at (a performance comparison needs them),
        // and what the frames since cost: each pass's wall and CPU time, this code's own CPU, the GPU time of its copies.
        if (passes >= fullPasses) {
            ++costWindow.frames;
            if (cfg.mvr && ab_run())
                ++ab.frames[ab_side()];
        }
        if (passes >= fullPasses && frames % 900 == 0) {
            unsigned mainWH[2];
            main_size(mainWH);
            if (cfg.mvr) {
                const MvrSizeCutStats c = mvr_size_cut_stats();
                simqvfr::logf("lmu qvfr: frame %llu: live resolution: eyes %ux%u %ux%u cut in %ux%u main targets (the "
                              "game's render scale %.3f); since launch %llu viewports and %llu scissors cut, %llu left",
                              frames, sceneW[0], sceneH[0], sceneW[1], sceneH[1], mainWH[0], mainWH[1], gameScale,
                              c.viewports, c.scissors, c.left);
            }
            {  // where the gaze is and where the focus boxes drawn this frame sit (whether the box follows the gaze)
                float gaze[2][2];
                bool valid[2];
                {
                    std::lock_guard guard(liveMutex);
                    std::memcpy(gaze, gazeTan, sizeof(gaze));
                    valid[0] = gazeValid[0];
                    valid[1] = gazeValid[1];
                }
                const auto centre = [](const FocusBox& b, int axis) {
                    return 0.5 * (b.focusTan[axis * 2] + b.focusTan[axis * 2 + 1]);
                };
                simqvfr::logf("lmu qvfr: frame %llu: gaze tangents %.3f %.3f%s | %.3f %.3f%s; the focus boxes drawn at "
                              "%.3f %.3f | %.3f %.3f (%s)",
                              frames, gaze[0][0], gaze[0][1], valid[0] ? "" : " (not valid)", gaze[1][0], gaze[1][1],
                              valid[1] ? "" : " (not valid)", centre(frozenBox[0], 0), centre(frozenBox[0], 1),
                              centre(frozenBox[1], 0), centre(frozenBox[1], 1),
                              buildFree ? "build-free" : "the game's facts");
            }
            std::uint32_t rec[2];
            real_recommended(rec);
            if (mainWH[0] && (mainWH[0] != rec[0] || mainWH[1] != rec[1]))  // render scale not 1: scene and eye target apart
                simqvfr::logf("lmu qvfr: frame %llu: passes rendered at %ux%u %ux%u %ux%u %ux%u in %ux%u main targets "
                              "(render scale %.3f, FSR %s), in the %ux%u eye targets %ux%u %ux%u %ux%u %ux%u",
                              frames, sceneW[0], sceneH[0], sceneW[1], sceneH[1], sceneW[2], sceneH[2], sceneW[3], sceneH[3],
                              mainWH[0], mainWH[1], gameScale, fsr_on() ? "on" : "off", viewW, viewH, passW[0], passH[0],
                              passW[1], passH[1], passW[2], passH[2], passW[3], passH[3]);
            else
                simqvfr::logf("lmu qvfr: frame %llu: passes rendered at %ux%u %ux%u %ux%u %ux%u in %ux%u targets", frames,
                              passW[0], passH[0], passW[1], passH[1], passW[2], passH[2], passW[3], passH[3], viewW, viewH);
            const CostWindow& w = costWindow;
            if (w.frames && tscPerMs > 0) {
                const double n = w.frames, g = w.gpuFrames ? w.gpuFrames : 1;
                const auto cpu = [&](double cycles) { return cycles / tscPerMs / n; };
                simqvfr::logf("lmu qvfr cost: %u frames: passes wall %.2f %.2f %.2f %.2f ms, CPU %.2f %.2f %.2f %.2f ms "
                              "(eyes %.2f, focus views %.2f ms CPU per frame); this code's CPU %.3f ms per frame; GPU of "
                              "its copies %.3f + %.3f + %.3f + %.3f = %.3f ms per frame (%u frames timed: eyes aside, focus 0, "
                              "focus 1 and eyes back, atlas)",
                              w.frames, w.passWallMs[0] / n, w.passWallMs[1] / n, w.passWallMs[2] / n, w.passWallMs[3] / n,
                              cpu(w.passCpuCycles[0]), cpu(w.passCpuCycles[1]), cpu(w.passCpuCycles[2]),
                              cpu(w.passCpuCycles[3]), cpu(w.passCpuCycles[0] + w.passCpuCycles[1]),
                              cpu(w.passCpuCycles[2] + w.passCpuCycles[3]), cpu(w.hookCycles), w.gpuMs[0] / g,
                              w.gpuMs[1] / g, w.gpuMs[2] / g, w.gpuMs[3] / g,
                              (w.gpuMs[0] + w.gpuMs[1] + w.gpuMs[2] + w.gpuMs[3]) / g, w.gpuFrames);
            }
            if (cfg.mvr && cfg.backendAb > 0 && backendAbOk && ab.frames[0] && ab.frames[1] && tscPerMs > 0) {
                double c[2][2];
                for (int m = 0; m < 2; ++m)
                    for (int q = 0; q < 2; ++q)
                        c[m][q] = ab.cpu[m][q] / tscPerMs / ab.frames[m];
                const MvrPassStats ms = mvr_pass_stats();
                simqvfr::logf("lmu mvr A/B (NVIDIA's vs instancing): CPU per pass with NVIDIA's %.2f %.2f "
                              "ms (%.2f; %u frames), with instancing %.2f %.2f ms (%.2f; %u frames): instancing %+.2f ms "
                              "per frame; %llu draws doubled so far, %llu into the eye alone",
                              c[0][0], c[0][1], c[0][0] + c[0][1], ab.frames[0], c[1][0], c[1][1], c[1][0] + c[1][1],
                              ab.frames[1], c[1][0] + c[1][1] - c[0][0] - c[0][1],
                              static_cast<unsigned long long>(ms.doubledDraws),
                              static_cast<unsigned long long>(ms.eyeOnlyDraws));
            }
            if (cfg.mvr && car_ab_on() && ab.frames[0] && ab.frames[1] && tscPerMs > 0) {
                double c[2][2];
                for (int m = 0; m < 2; ++m)
                    for (int q = 0; q < 2; ++q)
                        c[m][q] = ab.cpu[m][q] / tscPerMs / ab.frames[m];
                simqvfr::logf("lmu mvr A/B (other cars' motion): CPU per pass with it %.2f %.2f ms (%.2f; %u frames), "
                              "without %.2f %.2f ms (%.2f; %u frames): the cars' motion %+.3f ms per frame (the hooks' "
                              "detours on both sides)",
                              c[0][0], c[0][1], c[0][0] + c[0][1], ab.frames[0], c[1][0], c[1][1], c[1][0] + c[1][1],
                              ab.frames[1], c[0][0] + c[0][1] - c[1][0] - c[1][1]);
            }
            ab = AbWindow{};
            costWindow = CostWindow{};
        }
        if (frames % 1800 == 0)
            simqvfr::logf("lmu qvfr: frame %llu: %llu scene frames, %llu composed, %llu not (the eye images unlike the "
                          "views); last frame %d passes; views %ux%u; tracked passes on another eye %llu",
                          frames, sceneFrames, composedFrames, refusedFrames, passes, viewW, viewH,
                          trackedEyeMismatches);
    }

}

void qvfr_frame_cars_wanted(bool wanted) { carsWanted.store(wanted, std::memory_order_relaxed); }

void qvfr_frame_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]) {
    std::lock_guard guard(liveMutex);
    for (int e = 0; e < 2; ++e) {
        gazeValid[e] = valid[e] && std::isfinite(tan[e][0]) && std::isfinite(tan[e][1]);
        gazeTan[e][0] = tan[e][0];
        gazeTan[e][1] = tan[e][1];
    }
}

void qvfr_frame_set_box(double fractionX, double fractionY) {
    std::lock_guard guard(liveMutex);
    focusFraction = std::clamp(fractionX, 0.05, 1.0);
    focusFractionY = fractionY > 0 ? std::clamp(fractionY, 0.05, 1.0) : 0.0;
}

void qvfr_frame_set_focus_mask(bool on, const simqvfr::FocusArea& area, unsigned marginTexels) {
    std::lock_guard guard(liveMutex);
    focusMaskOn = on;
    focusMaskArea = area;
    focusMaskMargin = marginTexels;
}

simqvfr::games::QvfrMultiView qvfr_frame_multi_view() {
    return static_cast<simqvfr::games::QvfrMultiView>(multiViewNow.load(std::memory_order_relaxed));
}

bool qvfr_frame_eye_extent(unsigned (&width)[2], unsigned (&height)[2]) {
    if (!eyeExtentReady)
        return false;
    eyeExtentReady = false;  // this frame's
    for (int e = 0; e < 2; ++e) {
        width[e] = eyeExtent[e][0];
        height[e] = eyeExtent[e][1];
    }
    return true;
}

bool qvfr_frame_record(simqvfr::Views4Frame& frame) {
    std::lock_guard guard(liveMutex);
    if (!haveRecord)
        return false;
    frame = latestRecord;
    return true;
}

ID3D11Texture2D* qvfr_frame_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]) {
    if (off_render_thread()) {  // the copies would race the render thread on its context: the eyes as submitted
        if (!atlasThreadLogged) {
            atlasThreadLogged = true;
            simqvfr::log_line("lmu qvfr: xrEndFrame isn't on the thread that rendered the views: no atlas from it (the "
                              "headset shows the eyes that frame)");
        }
        atlasReady = menuReady = false;
        return nullptr;
    }
    if (cfg.trackerDrives)
        learn_eye_images(eyes);  // this frame's submit copies: the next frame's eye images
    if (qvfrMode && menuReady && menuAtlas) {  // a menu frame, at native (menu_ui)
        menuReady = false;
        std::memcpy(uv, menuUv, sizeof(uv));
        if (menuFrames++ == 0)
            simqvfr::logf("lmu qvfr: frame %llu: the first menu frame composed at %ux%u (its UI drawn at that size)", frames,
                          menuW, menuH);
        return menuAtlas;
    }
    if (!qvfrMode || !atlasReady || !atlas)
        return nullptr;
    atlasReady = false;  // once per frame
    if (!is_view(eyes[0]) || !is_view(eyes[1])) {
        ++refusedFrames;
        if (!atlasRefusalLogged) {
            atlasRefusalLogged = true;
            D3D11_TEXTURE2D_DESC d{};
            if (eyes[0])
                eyes[0]->GetDesc(&d);
            simqvfr::logf("lmu qvfr: the game's eye image is %ux%u format %d, the views %ux%u format %d: not composed "
                          "(the headset shows the eyes)",
                          d.Width, d.Height, static_cast<int>(d.Format), viewW, viewH, static_cast<int>(atlasFormat));
        }
        return nullptr;
    }
    // The eyes as the game submitted them, with what it drew after its eye loop (the VR UI), each only as far as its pass
    // rendered (live resolution: the top-left of the launch-size targets); the focus finals beside them, the VR UI
    // doubled onto them.
    const unsigned long long c0 = thread_cycles();
    gpuSpans.mark(3, 0);
    // The eye that's off: only the few pixels its pass drew (the headset shows it black).
    const auto eyeW = [](int e) { return off_pass(e) ? std::min(kOffEyeSize, passW[e]) : passW[e]; };
    const auto eyeH = [](int e) { return off_pass(e) ? std::min(kOffEyeSize, passH[e]) : passH[e]; };
    atlas_in(eyes[0], 0, 0, eyeW(0), eyeH(0));
    atlas_in(eyes[1], viewW, 0, eyeW(1), eyeH(1));
    atlas_in_from(eyes[0], finalAt[0][0], finalAt[0][1], 0, viewH, passW[2], passH[2]);
    atlas_in_from(eyes[1], finalAt[1][0], finalAt[1][1], viewW, viewH, passW[3], passH[3]);
    gpuSpans.mark(3, 1);
    gpuSpans.end_frame();
    costWindow.hookCycles += static_cast<double>(thread_cycles() - c0);
    // Each view's exact part of the atlas cell (extentW: below the game's render scale 1, the stretched part).
    const double cw = 0.5 / viewW, ch = 0.5 / viewH;
    for (int e = 0; e < 2; ++e) {
        const float outer[4] = {0.5f * e, 0.0f, static_cast<float>(cw * extentW[e]), static_cast<float>(ch * extentH[e])};
        const float focus[4] = {0.5f * e, 0.5f, static_cast<float>(cw * extentW[e + 2]),
                                static_cast<float>(ch * extentH[e + 2])};
        std::memcpy(uv[e], outer, sizeof(outer));
        std::memcpy(uv[e + 2], focus, sizeof(focus));
    }
    if (composedFrames++ == 0)
        simqvfr::logf("lmu qvfr: frame %llu: the first four views handed to the compositor", frames);
    const bool live = passW[0] != viewW || passW[2] != viewW || passH[0] != viewH || passH[2] != viewH;
    if (live != liveLogged) {
        liveLogged = live;
        simqvfr::logf("lmu qvfr: frame %llu: views rendered at outer %ux%u, focus %ux%u of %ux%u%s", frames, passW[0],
                      passH[0], passW[2], passH[2], viewW, viewH, live ? " (live resolution)" : " (the launch size)");
    }
    capture_views();
    return atlas;
}

void qvfr_frame_note_views(const XrView* views, uint32_t count) {
    if (!views || count < 2)
        return;
    std::lock_guard guard(liveMutex);
    bool stale = false;
    for (int e = 0; e < 2; ++e) {
        const XrFovf& f = views[e].fov;
        const float t[4] = {std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleDown), std::tan(f.angleUp)};
        std::memcpy(locatedTan[e], t, sizeof(t));
        if (maskKnown[e])
            for (int k = 0; k < 4; ++k)
                stale = stale || std::fabs(t[k] - maskTan[e][k]) > 2e-4f;
    }
    locatedValid = true;
    maskStale.store(stale, std::memory_order_relaxed);
}

bool qvfr_frame_known_build() { return exe_known_build(); }
bool qvfr_frame_build_free() { return buildFree; }
bool qvfr_frame_eye_projection(int eye, float (&out)[16]) { return eye >= 0 && eye < 2 && layer_slot(eye, out); }

void qvfr_frame_offered(std::uint32_t width, std::uint32_t height) {
    const bool changed = width != offeredW.load(std::memory_order_relaxed) ||
                         height != offeredH.load(std::memory_order_relaxed);
    offeredW.store(width, std::memory_order_relaxed);
    offeredH.store(height, std::memory_order_relaxed);
    if (changed)
        simqvfr::logf("lmu qvfr: the game offered %ux%u per eye (the eye targets' size%s)", width, height,
                      buildFree ? "; this run's, nothing read in the game" : "");
}

void qvfr_frame_game_views(const XrView* views, uint32_t count) {
    if (!views || count < 2)
        return;
    std::lock_guard guard(liveMutex);
    for (int e = 0; e < 2; ++e) {
        const XrFovf& f = views[e].fov;
        const float t[4] = {std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleDown), std::tan(f.angleUp)};
        std::memcpy(gameFovTan[e], t, sizeof(t));
    }
    gameFovValid = true;
}

void qvfr_frame_mask_taken(uint32_t viewIndex) {
    if (viewIndex >= 2)
        return;
    std::lock_guard guard(liveMutex);
    maskKnown[viewIndex] = locatedValid;
    std::memcpy(maskTan[viewIndex], locatedTan[viewIndex], sizeof(maskTan[viewIndex]));
    const float* t = maskTan[viewIndex];
    simqvfr::logf("lmu qvfr: eye %u's lens mask fetched: built with the FOV located last (tangents l %.3f r %.3f d %.3f "
                  "u %.3f)%s",
                  viewIndex, t[0], t[1], t[2], t[3], locatedValid ? "" : " -- none located yet: the mask is always drawn");
}

void qvfr_frame_set_live_scales(double outer, double focus) {
    const float o = static_cast<float>(std::clamp(outer, 0.1, 1.0)), f = static_cast<float>(std::clamp(focus, 0.1, 1.0));
    std::lock_guard guard(liveMutex);
    if (o == liveScale[0] && f == liveScale[1])
        return;
    liveScale[0] = o;
    liveScale[1] = f;
    simqvfr::logf("lmu qvfr: live resolution: outer views at %.0f%%, focus views at %.0f%% of the launch size per axis "
                  "(from the next frame)",
                  o * 100, f * 100);
}

void qvfr_frame_set_live_axes(double outerWidth, double outerHeight, double focusWidth, double focusHeight) {
    const auto share = [](double v) { return static_cast<float>(std::clamp(v, 0.05, 1.0)); };
    const float a[2][2] = {{share(outerWidth), share(outerHeight)}, {share(focusWidth), share(focusHeight)}};
    std::lock_guard guard(liveMutex);
    if (std::memcmp(a, liveAxes, sizeof(a)) == 0)
        return;
    std::memcpy(liveAxes, a, sizeof(a));
    if (cfg.perAxis)
        simqvfr::logf("lmu qvfr: live resolution per axis: outer views at %.0f%% x %.0f%%, focus views at %.0f%% x %.0f%% "
                      "of the target (from the next frame)",
                      a[0][0] * 100, a[0][1] * 100, a[1][0] * 100, a[1][1] * 100);
}

void qvfr_frame_view_size(int (&size)[2]) {
    size[0] = static_cast<int>(viewW);
    size[1] = static_cast<int>(viewH);
}

}  // namespace lmu
