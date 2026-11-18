// LMU's menus at native resolution (frames without the eye loop; the VR UI panel drawn again): qvfr_frame_internal.h.
#include "qvfr_frame_internal.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "../../log.h"
#include "../../quad/d3d11_view_resample.h"
#include "focus_math.h"
#include "mvr_pass.h"

namespace lmu::qvfr_frame_detail {

// ---- QVFR: the menus at native resolution ----
// A frame without the eye loop (the main menu, loading) is the game's eye images as they are, at the views' size: its
// UI as soft as the periphery. Instead each eye's image is scaled up to the native eye into an image of ours before the
// game draws its UI, the UI panel drawn into that at native after (as into the focus views), and the layer composes a
// menu atlas of the two (the focus views the same pixels). Kept for the session (the compositor holds views of them).
bool menuNative = false;        // QvfrFrameRun::menuNative
double layerShare[2] = {1, 1};  // the views' layer in native eye widths and heights (QvfrFrameRun)
simqvfr::D3D11ViewResample menuResample;
bool menuResampleTried = false;
ID3D11Texture2D* menuTex[2] = {};
ID3D11RenderTargetView* menuRtv[2] = {};
ID3D11Texture2D* menuAtlas = nullptr;
unsigned menuW = 0, menuH = 0;
bool menuReady = false;  // this frame's menu is in the menu atlas, until the layer takes it
float menuUv[4][4] = {};
unsigned long long menuFrames = 0;
bool menuRefusalLogged = false;

// RTVs on `textures` like the game's eye RTV `like`; false (none made) when they can't be.
bool make_rtvs(ID3D11DeviceContext* context, ID3D11RenderTargetView* like, ID3D11Texture2D* const (&textures)[2],
               ID3D11RenderTargetView* (&out)[2]) {
    for (auto*& view : out)
        if (view) {
            view->Release();
            view = nullptr;
        }
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    like->GetDesc(&d);
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    bool ok = d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D;
    for (int e = 0; e < 2; ++e)
        ok = ok && textures[e] && SUCCEEDED(device->CreateRenderTargetView(textures[e], &d, &out[e]));
    device->Release();
    if (!ok)
        for (auto*& view : out)
            if (view) {
                view->Release();
                view = nullptr;
            }
    return ok;
}


// The menus at native resolution (below): before and after the game's own UI draw in a frame without the eye loop.
bool menu_background(ID3D11DeviceContext* context, ID3D11RenderTargetView* like);

// The menu images (native: w x h, the eye texture `like`'s format) and the menu atlas (2w x h); false if they can't be.
bool ensure_menu(ID3D11DeviceContext* context, ID3D11Texture2D* like, ID3D11RenderTargetView* likeRtv, unsigned w,
                 unsigned h) {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!menuResample.ready() && !menuResampleTried) {
        menuResampleTried = true;
        if (!menuResample.initialize(device))
            simqvfr::log_line("lmu qvfr: the menu's scaler couldn't be made: menus at the views' size");
    }
    D3D11_TEXTURE2D_DESC d;
    like->GetDesc(&d);
    bool ok = menuResample.ready();
    if (ok && !(menuTex[0] && menuTex[1] && menuAtlas && menuRtv[0] && menuRtv[1] && menuW == w && menuH == h)) {
        for (int e = 0; e < 2; ++e) {
            if (menuRtv[e])
                menuRtv[e]->Release();
            if (menuTex[e])
                menuTex[e]->Release();
            menuRtv[e] = nullptr;
            menuTex[e] = nullptr;
        }
        if (menuAtlas)
            menuAtlas->Release();
        menuAtlas = nullptr;
        D3D11_TEXTURE2D_DESC t{};
        t.Width = w;
        t.Height = h;
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.Format = d.Format;
        t.SampleDesc = {1, 0};
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int e = 0; e < 2; ++e)
            ok = ok && SUCCEEDED(device->CreateTexture2D(&t, nullptr, &menuTex[e]));
        D3D11_TEXTURE2D_DESC a = t;
        a.Width = 2 * w;
        a.Format = typeless_of(d.Format);
        a.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&a, nullptr, &menuAtlas)) && make_rtvs(context, likeRtv, menuTex, menuRtv);
        menuW = ok ? w : 0;
        menuH = ok ? h : 0;
        simqvfr::logf(ok ? "lmu qvfr: menu images %ux%u (the native eye; the views %ux%u), menu atlas %ux%u"
                         : "lmu qvfr: the menu images %ux%u couldn't be made (the views %ux%u, atlas %ux%u): menus at the "
                           "views' size",
                      w, h, d.Width, d.Height, 2 * w, h);
    }
    device->Release();
    return ok;
}

bool menu_background(ID3D11DeviceContext* context, ID3D11RenderTargetView* like) {
    ID3D11Texture2D* eyes[2] = {game_eye_image(0), game_eye_image(1)};
    bool ok = like && eyes[0] && eyes[1];
    D3D11_TEXTURE2D_DESC d{};
    unsigned w = 0, h = 0;
    if (ok) {
        eyes[0]->GetDesc(&d);
        // The native eye: the size the layer offered over the layer's share of it.
        std::uint32_t recommended[2];
        real_recommended(recommended);
        w = std::min(8192u, static_cast<unsigned>(std::lround(recommended[0] / layerShare[0])));
        h = std::min(8192u, static_cast<unsigned>(std::lround(recommended[1] / layerShare[1])));
        ok = d.SampleDesc.Count == 1 && (w > d.Width || h > d.Height);  // else the eyes are as sharp already
    }
    ok = ok && ensure_menu(context, eyes[0], like, w, h);
    if (ok) {
        // The same tangents both sides: a straight bilinear scale of the whole eye image.
        const simqvfr::ViewMapping same{{-1, 1, 1, -1}, {-1, 1, 1, -1}, {1, 0, 0, 0, 1, 0, 0, 0, 1}};
        for (int e = 0; e < 2; ++e)
            ok = ok && menuResample.resample_target(eyes[e], {0, 0, static_cast<int>(d.Width), static_cast<int>(d.Height)},
                                                    menuTex[e], {0, 0, static_cast<int>(menuW), static_cast<int>(menuH)},
                                                    same);
        if (!ok && !menuRefusalLogged) {
            menuRefusalLogged = true;
            simqvfr::log_line("lmu qvfr: the menu background couldn't be scaled up: menus at the views' size");
        }
    }
    for (ID3D11Texture2D* t : eyes)
        if (t)
            t->Release();
    return ok;
}

// A menu frame's record: the eyes' own projections, each focus view its box's part of the eye (the same pixels), and
// the views' places in the menu atlas. False: the eyes as they are.
bool menu_record(simqvfr::Views4Frame& f, float (&uv)[4][4]) {
    double fx, fy, cx[2] = {}, cy[2] = {};
    {
        std::lock_guard guard(liveMutex);
        fx = focusFraction;
        fy = focusFractionY > 0 ? focusFractionY : focusFraction;
        for (int e = 0; e < 2; ++e)
            if (gazeValid[e]) {
                cx[e] = gazeTan[e][0];
                cy[e] = gazeTan[e][1];
            }
    }
    f = {};
    for (int e = 0; e < 2; ++e) {
        float eyeM[16], focusM[16];
        if (!slot_matrix(e, 0, eyeM))
            return false;
        FocusBox box;
        clamp_focus_centre(eyeM, fx, fy, cx[e], cy[e]);
        if (!focus_projection(eyeM, fx, fy, cx[e], cy[e], box, focusM))
            return false;
        const float outer[4] = {box.eyeTan[0], box.eyeTan[1], box.eyeTan[3], box.eyeTan[2]};
        const float focus[4] = {box.focusTan[0], box.focusTan[1], box.focusTan[3], box.focusTan[2]};
        std::memcpy(f.outerTan[e], outer, sizeof(outer));
        std::memcpy(f.focusTan[e], focus, sizeof(focus));
        f.turn[e] = {0, 0, 0, 1};
        const float o[4] = {0.5f * e, 0.0f, 0.5f, 1.0f};
        const float b[4] = {0.5f * e + 0.5f * static_cast<float>(box.rect[0]), static_cast<float>(box.rect[1]),
                            0.5f * static_cast<float>(box.rect[2] - box.rect[0]), static_cast<float>(box.rect[3] - box.rect[1])};
        std::memcpy(uv[e], o, sizeof(o));
        std::memcpy(uv[e + 2], b, sizeof(b));
    }
    f.live = true;
    for (int v = 0; v < 4; ++v) {
        f.width[v] = f.renderWidth[v] = 0.5;
        f.height[v] = f.renderHeight[v] = 1.0;
    }
    return true;
}

// The menu images (their UI drawn) into the menu atlas, the record stored: the layer composes the menu (qvfr_frame_atlas).
void menu_publish(const simqvfr::Views4Frame& f, const float (&uv)[4][4]) {
    ID3D11Device* device = nullptr;
    menuAtlas->GetDevice(&device);
    ID3D11DeviceContext* immediate = nullptr;
    device->GetImmediateContext(&immediate);
    for (int e = 0; e < 2; ++e)
        immediate->CopySubresourceRegion(menuAtlas, 0, e * menuW, 0, 0, menuTex[e], 0, nullptr);
    immediate->Release();
    device->Release();
    std::memcpy(menuUv, uv, sizeof(uv));
    {
        std::lock_guard guard(liveMutex);
        latestRecord = f;
        haveRecord = true;
    }
    menuReady = true;
}

// R6b, the multi-view path: the menus at native from the d3d11 calls alone (mvr_pass.h mvr_panel_draws), no call into
// the game's code. In a frame without passes, before the first VR UI panel draw into an eye's image the eyes are scaled
// up into the menu images (menu_background); after each panel draw the same draw into that eye's menu image at its size
// (mvr_redraw_panel); at the frame's end, both eyes' drawn, the atlas and the record (menu_finish).
bool menuFrameStarted = false, menuBackgroundOk = false, menuPanelDrawn[2] = {};
unsigned long long menuPanelReplays = 0, menuPanelMisses = 0;
bool menuPanelLogged = false;

void panel_draw(ID3D11DeviceContext* context, bool after, unsigned count, unsigned start) {
    if (!qvfrMode || !menuNative || passesThisFrame != 0)
        return;
    ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv)
        return;
    ID3D11Resource* target = nullptr;
    rtv->GetResource(&target);
    int eye = -1;
    for (int e = 0; e < 2; ++e) {
        ID3D11Texture2D* t = game_eye_image(e);
        if (t && target && static_cast<ID3D11Resource*>(t) == target)
            eye = e;
        if (t)
            t->Release();
    }
    if (target)
        target->Release();
    if (eye >= 0 && !after && !menuFrameStarted) {
        menuFrameStarted = true;
        menuBackgroundOk = menu_background(context, rtv);
    } else if (eye >= 0 && after && menuBackgroundOk && menuRtv[eye]) {
        mvr_redraw_panel(context, menuRtv[eye], menuW, menuH, count, start);
        menuPanelDrawn[eye] = true;
        ++menuPanelReplays;
        if (!menuPanelLogged) {
            menuPanelLogged = true;
            simqvfr::logf("lmu qvfr: frame %llu: the VR UI's panel drawn again into the menu images at %ux%u (from its "
                          "d3d11 draw)",
                          frames, menuW, menuH);
        }
    } else if (eye < 0 && after) {
        ++menuPanelMisses;  // a panel into something else than an eye's image
    }
    rtv->Release();
}

// At the frame's end (xrEndFrame, before the layer takes the atlas): a menu frame whose panels were drawn into both menu
// images is published.
void menu_finish() {
    const bool both = menuPanelDrawn[0] && menuPanelDrawn[1];
    menuFrameStarted = menuBackgroundOk = menuPanelDrawn[0] = menuPanelDrawn[1] = false;
    if (!both || !menuAtlas)
        return;
    simqvfr::Views4Frame f{};
    float uv[4][4];
    if (menu_record(f, uv))
        menu_publish(f, uv);
}

}  // namespace lmu::qvfr_frame_detail
