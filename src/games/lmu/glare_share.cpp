#include "glare_share.h"

#include <windows.h>
#include <d3d11.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>

#include "../../engine/code_patch.h"
#include "../../log.h"
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
double currentShare[2] = {};  // of its target the pass renders (for the region log; 0 not known yet)
unsigned long long frameId = 0;
ID3D11Texture2D* eyeGlare[2] = {};
simqvfr::ViewRegion eyeRegion[2];
unsigned long long eyeFrame[2] = {};
simqvfr::D3D11ViewResample resample;
ID3D11Texture2D* lastGlareTexture = nullptr;  // YEBIS's (not held): its views are dropped when it changes

struct Counts {
    unsigned long long kept = 0, shared = 0, noEye = 0, noBox = 0, noShare = 0, unread = 0, implausible = 0, failed = 0;
} counts;
bool readLogged = false, implausibleLogged = false, failLogged = false, firstShareLogged = false;
simqvfr::ViewRegion loggedRegion[2];  // the eyes' and the focus views' regions as last logged

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
    simqvfr::logf("lmu qvfr: glare share: the %s' glare is %dx%d of YEBIS's %ux%u texture (format %d): the pass rendered "
                  "%.4f x %.4f of its target, of the %dx%d YEBIS uses (render texture %ux%u, UV extent %.4f x %.4f%s)",
                  which ? "focus views" : "eyes", r.width, r.height, g.textureWidth, g.textureHeight,
                  static_cast<int>(d.Format), currentShare[0], currentShare[1], yebisRegion.width, yebisRegion.height,
                  g.width, g.height, ux, uy,
                  std::fabs(ux - 1) < 0.002 && std::fabs(uy - 1) < 0.002 ? "" : "; not 1 as in the runs so far: check");
}

void after_composite(void* renderGlare) {
    YebisGlare g;
    const int error = read_yebis_glare(renderGlare, g);
    if (error) {
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
    if (region.width == 0) {  // the pass's share not known yet (the first four-pass frame's eyes)
        ++counts.noShare;
        g.texture->Release();
        return;
    }
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
    } else {
        const int e = currentPass - 2;
        log_region(1, g, yebisRegion, region);
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
            if (resample.resample_target(eyeGlare[e], eyeRegion[e], g.texture, region, glare_mapping(currentBox))) {
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
    }
    g.texture->Release();
}

std::int64_t glare_composite(void* renderGlare) {
    const std::int64_t result = originalComposite(renderGlare);
    if (currentPass >= 0 && static_cast<std::int32_t>(result) >= 0 && sharing.load(std::memory_order_relaxed) &&
        GetCurrentThreadId() == passThread)
        after_composite(renderGlare);
    return result;
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

void glare_share_pass(int pass, const FocusBox* box, double shareX, double shareY) {
    passThread = GetCurrentThreadId();
    currentPass = pass;
    currentShare[0] = shareX;
    currentShare[1] = shareY;
    if (pass == 0)
        ++frameId;
    currentBoxValid = box && box->valid;
    if (currentBoxValid)
        currentBox = *box;
}

void glare_share_frame_end(unsigned long long frame) {
    if (!installed || frame % 900 != 0 || (counts.kept == 0 && counts.unread == 0 && counts.implausible == 0))
        return;
    simqvfr::logf("lmu qvfr: glare share: frame %llu (%s): %llu eye glares kept, %llu focus glares replaced by their "
                  "eye's; not replaced: %llu (no eye glare this frame), %llu (no focus box), %llu (the pass's size not "
                  "known yet), %llu (unread), %llu (region not plausible), %llu (resample failed)",
                  frame, sharing.load() ? "on" : "off", counts.kept, counts.shared, counts.noEye, counts.noBox,
                  counts.noShare, counts.unread, counts.implausible, counts.failed);
}

}  // namespace lmu
