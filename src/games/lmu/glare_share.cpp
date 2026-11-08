#include "glare_share.h"

#include <windows.h>
#include <d3d11.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "../../probe/d3d11_capture.h"
#include "../../quad/d3d11_view_resample.h"
#include "abi.h"
#include "glare_math.h"

namespace lmu {
namespace {

using CompositeFn = std::int64_t (*)(void* renderGlare);
CompositeFn originalComposite = nullptr;
bool installed = false;
std::atomic<bool> sharing{true};

// The render thread's pass (glare_share_pass) and this frame's eye glares.
unsigned long passThread = 0;
int currentPass = -1;
FocusBox currentBox;
bool currentBoxValid = false;
unsigned passSize[2] = {}, viewSize[2] = {};  // what the pass renders of its target (0 not known yet)
double currentShare[2] = {};                  // their ratio per axis
double currentScale = 0;                      // YEBIS's processed share of the target (0 not known)
unsigned long long frameId = 0;
ID3D11Texture2D* eyeGlare[2] = {};
simqvfr::ViewRegion eyeRegion[2];
unsigned long long eyeFrame[2] = {};
simqvfr::D3D11ViewResample resample;
ID3D11Texture2D* lastGlareTexture = nullptr;  // YEBIS's (not held): its views are dropped when it changes

struct Counts {
    unsigned long long kept = 0, shared = 0, noEye = 0, noBox = 0, noShare = 0, unread = 0, implausible = 0, failed = 0;
    unsigned long long uncovered = 0;  // passes whose view reaches past YEBIS's processed square (yebis_covers)
} counts;
bool readLogged = false, implausibleLogged = false, failLogged = false, firstShareLogged = false;
simqvfr::ViewRegion loggedRegion[2];  // the eyes' and the focus views' regions as last logged

// The frame capture hotkey (glare_share_capture_frame): each pass's glare texture, read back after the composite.
struct Shot {
    bool taken = false;
    std::vector<std::uint8_t> texels;
    unsigned width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    simqvfr::ViewRegion region;  // the view's (yebis_pass_region)
    unsigned pass[2] = {}, view[2] = {};
    double scale = 0;
    bool shareOn = false;  // the share was on for this pass
    bool shared = false;   // a focus pass's after the share replaced its region
};
Shot shots[4], ownShots[2];          // ownShots: a focus pass's own glare, before the share replaced it
bool captureNext = false;            // set before pass 0: that frame is read back
unsigned long long captureFor = 0;   // the frameId being read back (0: none)

void take(Shot& s, ID3D11Texture2D* texture, const simqvfr::ViewRegion& region, bool shareOn, bool shared) {
    s.taken = simqvfr::d3d11_read_texels32(texture, s.texels, s.width, s.height, s.format);
    s.region = region;
    for (int i = 0; i < 2; ++i) {
        s.pass[i] = passSize[i];
        s.view[i] = viewSize[i];
    }
    s.scale = currentScale;
    s.shareOn = shareOn;
    s.shared = shared;
}

// `dst` (made again when the size or format differs) = the top-left `region` of `src`.
bool keep(ID3D11Texture2D*& dst, ID3D11Texture2D* src, const simqvfr::ViewRegion& region) {
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    if (dst) {
        D3D11_TEXTURE2D_DESC have;
        dst->GetDesc(&have);
        if (have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
            dst->Release();
            dst = nullptr;
        }
    }
    ID3D11Device* device = nullptr;
    src->GetDevice(&device);
    if (!dst) {
        D3D11_TEXTURE2D_DESC aside{d.Width, d.Height, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
        if (FAILED(device->CreateTexture2D(&aside, nullptr, &dst)))
            dst = nullptr;
    }
    if (dst) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(region.width), static_cast<UINT>(region.height), 1};
        context->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, &box);
        context->Release();
    }
    device->Release();
    return dst != nullptr;
}

void log_region(int which, const YebisGlare& g, const simqvfr::ViewRegion& yebisRegion, const simqvfr::ViewRegion& r) {
    simqvfr::ViewRegion& last = loggedRegion[which];
    if (last.width == r.width && last.height == r.height)
        return;
    last = r;
    D3D11_TEXTURE2D_DESC d;
    g.texture->GetDesc(&d);
    const double ux = yebis_uv_extent(g.width, g.scaleOffset[0], g.scaleOffset[2]);
    const double uy = yebis_uv_extent(g.height, g.scaleOffset[1], g.scaleOffset[3]);
    const bool covered = yebis_covers(passSize[0], passSize[1], viewSize[0], viewSize[1], currentScale);
    simqvfr::logf("lmu qvfr: glare share: the %s' glare is %dx%d of YEBIS's %ux%u texture (format %d): the pass rendered "
                  "%ux%u of its %ux%u target (%.4f x %.4f), YEBIS post-processed %.4f of it on both axes (render "
                  "scale)%s; of the %dx%d YEBIS uses (render texture %ux%u, UV extent %.4f x %.4f%s)",
                  which ? "focus views" : "eyes", r.width, r.height, g.textureWidth, g.textureHeight,
                  static_cast<int>(d.Format), passSize[0], passSize[1], viewSize[0], viewSize[1], currentShare[0],
                  currentShare[1], currentScale,
                  !(currentScale > 0) ? " (not known yet)"
                  : covered           ? ""
                                      : ": THE VIEW REACHES PAST IT, its glare there isn't this pass's",
                  yebisRegion.width,
                  yebisRegion.height, g.width, g.height, ux, uy,
                  std::fabs(ux - 1) < 0.002 && std::fabs(uy - 1) < 0.002 ? "" : "; not 1 as in the runs so far: check");
}

// After YEBIS's composite: with the share on, an eye pass's glare kept and a focus pass's replaced; at a frame capture
// (share on or off) each pass's glare read back too.
void after_composite(void* renderGlare, bool share) {
    const bool capture = captureFor != 0 && captureFor == frameId && currentPass < 4;
    YebisGlare g;
    const int error = read_yebis_glare(renderGlare, g);
    if (error) {
        if (!share)
            return;
        ++counts.unread;
        if (!readLogged) {
            readLogged = true;
            simqvfr::logf("lmu qvfr: glare share: pass %d: YEBIS's glare result can't be had (%s); the focus views keep "
                          "their own glare",
                          currentPass,
                          error == 1   ? "a null pointer on the way"
                          : error == 2 ? "unreadable memory on the way"
                                       : "not a 2D texture at least the size its render texture says");
        }
        return;
    }
    const simqvfr::ViewRegion yebisRegion =
        yebis_region(g.textureWidth, g.textureHeight, g.width, g.height, g.scaleOffset);
    // The pass's view: the share of its target it rendered (live resolution), as YEBIS samples the glare at the scene's UV.
    const simqvfr::ViewRegion region = yebis_pass_region(yebisRegion, currentShare[0], currentShare[1]);
    if (!share) {  // a frame capture with the share off: YEBIS's own glare, and the region the share would use
        if (capture && region.width > 0)
            take(shots[currentPass], g.texture, region, false, false);
        g.texture->Release();
        return;
    }
    if (region.width == 0) {  // the pass's share not known yet (the first four-pass frame's eyes)
        ++counts.noShare;
        g.texture->Release();
        return;
    }
    if (currentScale > 0 && !yebis_covers(passSize[0], passSize[1], viewSize[0], viewSize[1], currentScale))
        ++counts.uncovered;
    if (!yebis_region_plausible(region, g.textureWidth, g.textureHeight)) {
        ++counts.implausible;
        if (!implausibleLogged) {
            implausibleLogged = true;
            simqvfr::logf("lmu qvfr: glare share: pass %d: YEBIS's glare region reads as %dx%d of its %ux%u texture (render "
                          "texture %ux%u, UV scale %.6f %.6f, offset %.4f %.4f): not plausible, nothing shared",
                          currentPass, region.width, region.height, g.textureWidth, g.textureHeight, g.width, g.height,
                          g.scaleOffset[0], g.scaleOffset[1], g.scaleOffset[2], g.scaleOffset[3]);
        }
        g.texture->Release();
        return;
    }
    if (g.texture != lastGlareTexture) {
        lastGlareTexture = g.texture;
        resample.forget_views();
    }
    if (currentPass < 2) {
        log_region(0, g, yebisRegion, region);
        if (keep(eyeGlare[currentPass], g.texture, region)) {
            eyeRegion[currentPass] = {0, 0, region.width, region.height};
            eyeFrame[currentPass] = frameId;
            ++counts.kept;
        }
        if (capture)
            take(shots[currentPass], g.texture, region, true, false);
    } else {
        const int e = currentPass - 2;
        log_region(1, g, yebisRegion, region);
        bool replaced = false;
        if (!currentBoxValid) {
            ++counts.noBox;
        } else if (eyeFrame[e] != frameId || !eyeGlare[e]) {
            ++counts.noEye;
        } else {
            if (!resample.ready()) {
                ID3D11Device* device = nullptr;
                g.texture->GetDevice(&device);
                resample.initialize(device);
                device->Release();
            }
            if (capture)
                take(ownShots[e], g.texture, region, true, false);
            replaced = resample.resample_target(eyeGlare[e], eyeRegion[e], g.texture, region, glare_mapping(currentBox));
            if (replaced) {
                ++counts.shared;
                if (!firstShareLogged) {
                    firstShareLogged = true;
                    const float* f = currentBox.focusTan;
                    simqvfr::logf("lmu qvfr: glare share: frame %llu: focus view %d took eye %d's glare (its %dx%d through "
                                  "the box l %.3f r %.3f d %.3f u %.3f into %dx%d)",
                                  frameId, e, e, eyeRegion[e].width, eyeRegion[e].height, f[0], f[1], f[2], f[3],
                                  region.width, region.height);
                }
            } else {
                ++counts.failed;
                if (!failLogged) {
                    failLogged = true;
                    simqvfr::log_line("lmu qvfr: glare share: the resample into YEBIS's glare failed (its render target "
                                      "view, or the shaders); the focus views keep their own glare");
                }
            }
        }
        if (capture)
            take(shots[currentPass], g.texture, region, true, replaced);
    }
    g.texture->Release();
}

std::int64_t glare_composite(void* renderGlare) {
    const std::int64_t result = originalComposite(renderGlare);
    if (currentPass >= 0 && static_cast<std::int32_t>(result) >= 0 && GetCurrentThreadId() == passThread) {
        const bool share = sharing.load(std::memory_order_relaxed);
        if (share || (captureFor != 0 && captureFor == frameId))
            after_composite(renderGlare, share);
    }
    return result;
}

// One read-back glare as a DDS file and a log line: where the view is in it, and the command that checks it.
void save_shot(const Shot& s, int pass, const std::wstring& path, const std::wstring& viewPath, const char* what) {
    static const char* const names[4] = {"eye 0", "eye 1", "focus view 0", "focus view 1"};
    if (!s.taken) {
        simqvfr::logf("lmu qvfr: glare capture: pass %d's glare (%s) wasn't read back", pass, what);
        return;
    }
    if (!simqvfr::write_dds(path, s.texels, s.width, s.height, s.format)) {
        simqvfr::logf("lmu qvfr: glare capture: %ls not written", path.c_str());
        return;
    }
    char check[2048] = "";
    if (!viewPath.empty())
        std::snprintf(check, sizeof(check), ": python tools/glare_place.py --image \"%ls\" --glare \"%ls\" --region %d %d",
                      viewPath.c_str(), path.c_str(), s.region.width, s.region.height);
    simqvfr::logf("lmu qvfr: glare capture: pass %d (%s), %s: %ls, %ux%u format %d, the view in its top-left %dx%d (the "
                  "pass rendered %ux%u of %ux%u, YEBIS post-processed %.4f)%s",
                  pass, names[pass], what, path.c_str(), s.width, s.height, static_cast<int>(s.format), s.region.width,
                  s.region.height, s.pass[0], s.pass[1], s.view[0], s.view[1], s.scale, check);
}

}  // namespace

bool glare_share_start() {
    if (installed)
        return true;
    if (!simqvfr::code_matches(kGlareComposite, kGlareCompositeAnchor, sizeof(kGlareCompositeAnchor))) {
        simqvfr::log_line("lmu qvfr: glare share: YEBIS's glare composite isn't this build's; the focus views keep their "
                          "own glare");
        return false;
    }
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    void* target = reinterpret_cast<void*>(simqvfr::module_base() + kGlareComposite);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&glare_composite), reinterpret_cast<void**>(&originalComposite)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        simqvfr::log_line("lmu qvfr: glare share: YEBIS's glare composite couldn't be hooked; the focus views keep their "
                          "own glare");
        return false;
    }
    installed = true;
    return true;
}

void glare_share_set(bool on) {
    if (installed && sharing.exchange(on) != on)
        simqvfr::logf("lmu qvfr: glare share %s (live)", on ? "on: the focus views take their eye's glare"
                                                            : "off: each view's own glare (YEBIS as the game runs it)");
}

bool glare_share_on() { return installed && sharing.load(); }

void glare_share_pass(int pass, const FocusBox* box, unsigned passW, unsigned passH, unsigned viewW, unsigned viewH,
                      double yebisScale) {
    passThread = GetCurrentThreadId();
    currentPass = pass;
    passSize[0] = passW;
    passSize[1] = passH;
    viewSize[0] = viewW;
    viewSize[1] = viewH;
    currentShare[0] = viewW ? static_cast<double>(passW) / viewW : 0.0;
    currentShare[1] = viewH ? static_cast<double>(passH) / viewH : 0.0;
    currentScale = yebisScale;
    if (pass == 0) {
        ++frameId;
        if (captureNext) {
            captureNext = false;
            captureFor = frameId;
            for (Shot& s : shots)
                s.taken = false;
            for (Shot& s : ownShots)
                s.taken = false;
        }
    }
    currentBoxValid = box && box->valid;
    if (currentBoxValid)
        currentBox = *box;
}

void glare_share_frame_end(unsigned long long frame) {
    if (!installed || frame % 900 != 0 || (counts.kept == 0 && counts.unread == 0 && counts.implausible == 0))
        return;
    simqvfr::logf("lmu qvfr: glare share: frame %llu (%s): %llu eye glares kept, %llu focus glares replaced by their "
                  "eye's; not replaced: %llu (no eye glare this frame), %llu (no focus box), %llu (the pass's size not "
                  "known yet), %llu (unread), %llu (region not plausible), %llu (resample failed); %llu passes reached "
                  "past YEBIS's processed square",
                  frame, sharing.load() ? "on" : "off", counts.kept, counts.shared, counts.noEye, counts.noBox,
                  counts.noShare, counts.unread, counts.implausible, counts.failed, counts.uncovered);
}

void glare_share_capture_frame() {
    if (installed)
        captureNext = true;
}

void glare_share_save_capture(const std::wstring& dir, const std::wstring& stem, const std::wstring (&viewPaths)[4]) {
    if (captureFor == 0 || captureFor != frameId)
        return;  // this frame read none
    captureFor = 0;
    for (int p = 0; p < 4; ++p) {
        const Shot& s = shots[p];
        const char* what = p < 2 ? (s.shareOn ? "YEBIS's (share on: kept)" : "YEBIS's (share off)")
                           : !s.shareOn ? "its own (share off)"
                           : s.shared ? "after the share (its eye's)"
                                      : "its own (share on but not replaced)";
        save_shot(s, p, (std::filesystem::path(dir) / (stem + L"-glare" + std::to_wstring(p) + L".dds")).wstring(),
                  viewPaths[p], what);
    }
    for (int e = 0; e < 2; ++e)
        if (ownShots[e].taken)
            save_shot(ownShots[e], 2 + e,
                      (std::filesystem::path(dir) / (stem + L"-glare" + std::to_wstring(2 + e) + L"-own.dds")).wstring(),
                      std::wstring(), "its own, before the share");
}

}  // namespace lmu
