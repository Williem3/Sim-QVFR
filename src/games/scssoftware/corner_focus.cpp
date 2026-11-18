// The focus view: NVIDIA's two views or instancing for its scene draws, the full-screen passes replayed for its
// region, and the twins those replays read (corner_internal.h).
#include "corner_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../engine/nvapi_stereo.h"
#include "../lmu/mvr_probe.h"
#include "../../log.h"

namespace scs {
namespace detail {

// Twins and twin views no draw has used for kEvictFrames frames, let go: the game made new targets (a menu, a loading
// pause: ETS2 2026-11-15, 11 twins and 112 MB grew to 33 and 337 MB), and a twin view holds the game's old texture
// alive with it. Used ones are touched every frame (twin_of, twin_view, mark_bound_written).
void evict_unused() {
    const uint64_t now = g.stats.frames;
    const auto stale = [now](uint64_t used) { return now - used > kEvictFrames; };
    size_t views = 0, twins = 0;
    for (auto it = g.twinViews.begin(); it != g.twinViews.end();)
        if (stale(it->second.usedFrame)) {
            it = g.twinViews.erase(it);
            ++views;
        } else {
            ++it;
        }
    for (auto it = g.texes.begin(); it != g.texes.end();)
        if (stale(it->second.usedFrame)) {
            g.twinBytes -= std::min(g.twinBytes, it->second.bytes);
            twins += it->second.twin ? 1 : 0;
            it = g.texes.erase(it);
        } else {
            ++it;
        }
    if (views || twins)
        simqvfr::logf("scs focus: %zu twins and %zu twin views unused for %llu frames let go (%zu twins left, %.0f MB at "
                      "most)",
                      twins, views, static_cast<unsigned long long>(kEvictFrames), g.texes.size(),
                      g.twinBytes / 1048576.0);
}

void release_orphans() {
    if (g.active)
        return;
    evict_mirrors(true);
    // Twin views first: each holds the game's view, and the view its texture. A texture let go takes its twin views
    // with it, so a view the game still binds can't point at a twin no entry fills any more.
    size_t views = 0, twins = 0;
    for (auto it = g.twinViews.begin(); it != g.twinViews.end();)
        if (only_ours(it->second.game.Get())) {
            it = g.twinViews.erase(it);
            ++views;
        } else {
            ++it;
        }
    for (auto it = g.texes.begin(); it != g.texes.end();) {
        if (!only_ours(it->second.game.Get())) {
            ++it;
            continue;
        }
        ID3D11Texture2D* const gone = it->second.game.Get();
        for (auto v = g.twinViews.begin(); v != g.twinViews.end();) {
            ComPtr<ID3D11Resource> r;
            v->second.game->GetResource(&r);
            if (r.Get() == gone) {
                v = g.twinViews.erase(v);
                ++views;
            } else {
                ++v;
            }
        }
        g.twinBytes -= std::min(g.twinBytes, it->second.bytes);
        twins += it->second.twin ? 1 : 0;
        it = g.texes.erase(it);
    }
    if (views || twins)
        simqvfr::logf("scs focus: %zu twins and %zu twin views whose game textures are gone let go (%zu twins left, %.0f MB "
                      "at most)",
                      twins, views, g.texes.size(), g.twinBytes / 1048576.0);
}

// The bound targets' focus regions are being written (a two-view scene draw, a replay).
void mark_bound_written() {
    if (g.boundMarked)
        return;
    g.boundMarked = true;
    for (ID3D11Texture2D* t : g.boundRt)
        if (t) {
            State::Tex& x = g.texes[t];
            if (!x.game)
                x.game = t;  // held: the key stays this texture's (bound now, so alive)
            ++x.writeSerial;
            x.writeFrame = g.stats.frames;
            x.usedFrame = g.stats.frames;
        }
}

// A texture with no focus content of its own (the bloom: one chain for the whole eye): its twin's region filled from the
// eye's image of the box, magnified, so the focus view takes the eye's (bloom, anything else of the eye's alone).
// Float and normalised formats only. The game's state is put back.
// `extent`: how far the fill goes, in focus views (1: the focus region alone; more: the eye's picture of what lies right
// of and below the box, so a pass that reaches past the region's edge finds the scene there, not nothing).
bool stretch_into_twin(State::Tex& t, ID3D11Texture2D* game, const D3D11_TEXTURE2D_DESC& d, float extent) {
    switch (d.Format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT: break;
        default: return false;
    }
    if (!blit_resources())
        return false;
    const float bx = beta_x(), by = beta_y();
    const float* b = g.box[current_eye()];
    const UINT x = static_cast<UINT>(std::lround(g.fx * d.Width * b[0])), y = static_cast<UINT>(std::lround(g.fy * d.Height * b[1]));
    extent = std::min({extent, (1 - b[0]) / bx, (1 - b[1]) / by});  // no further than the eye's own edge
    {
        const D3D11_RECT r = focus_rect_in(d.Width, d.Height);  // nor than the twin's
        extent = std::min(extent, std::min(static_cast<float>(twin_w(d.Width)) / std::max<LONG>(1, r.right - r.left),
                                           static_cast<float>(twin_h(d.Height)) / std::max<LONG>(1, r.bottom)));
    }
    extent = std::max(1.0f, extent);
    const UINT w = std::max<UINT>(1, static_cast<UINT>(std::lround(g.fx * d.Width * bx * extent)));
    const UINT h = std::max<UINT>(1, static_cast<UINT>(std::lround(g.fy * d.Height * by * extent)));
    if (x + w > d.Width || y + h > d.Height)
        return false;
    State::Stretch* st = nullptr;
    for (State::Stretch& e : g.stretches)
        if (e.desc.Width == w && e.desc.Height == h && e.desc.Format == d.Format)
            st = &e;
    if (!st) {
        State::Stretch e;
        e.desc = {w, h, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(g.device->CreateTexture2D(&e.desc, nullptr, &e.tex)) ||
            FAILED(g.device->CreateShaderResourceView(e.tex.Get(), nullptr, &e.view)))
            return false;
        g.stretches.push_back(e);
        st = &g.stretches.back();
    }
    ID3D11DeviceContext1* c = g.context.Get();
    const D3D11_BOX box = {x, y, 0, x + w, y + h, 1};
    c->CopySubresourceRegion(st->tex.Get(), 0, 0, 0, 0, game, 0, &box);
    // The game's state this draw touches, put back when this returns. GS and tessellation nulled for the blit (as
    // upscale): a game's would reshape it.
    const simqvfr::D3D11StateGuard keep(c, blit_state(d3s::kInputLayout | d3s::kVS | d3s::kPS | d3s::kGsHsDs |
                                                      d3s::kOMTargets | d3s::kBlend | d3s::kDepth | d3s::kRSState |
                                                      d3s::kViewports | d3s::kScissors));

    const D3D11_RECT f = focus_rect_in(d.Width, d.Height);
    const float ow = (f.right - f.left) * extent, oh = f.bottom * extent;
    const D3D11_VIEWPORT out = {0, 0, ow, oh, 0, 1};
    const D3D11_RECT outRect = {0, 0, static_cast<LONG>(std::ceil(ow)), static_cast<LONG>(std::ceil(oh))};
    ID3D11RenderTargetView* target[] = {t.twinRtv.Get()};
    ID3D11ShaderResourceView* src[] = {st->view.Get()};
    ID3D11SamplerState* linear[] = {g.linear.Get()};
    c->OMSetRenderTargets(1, target, nullptr);
    c->RSSetViewports(1, &out);
    c->RSSetScissorRects(1, &outRect);
    c->RSSetState(g.raster.Get());
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g.blitVs.Get(), nullptr, 0);
    c->PSSetShader(g.blitPs.Get(), nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->PSSetShaderResources(0, 1, src);
    c->PSSetSamplers(0, 1, linear);
    c->Draw(3, 0);

    ++g.stretched;
    return true;
}

// A game texture's twin, its top-left holding the texture's focus region as last written (nothing where the region
// has no focus content: zero). Null: no twin.
State::Tex* twin_of(ID3D11Texture2D* game, const D3D11_TEXTURE2D_DESC& d) {
    State::Tex& t = g.texes[game];
    t.usedFrame = g.stats.frames;
    if (!t.game)
        t.game = game;  // held by every entry, a failed one too: the key stays this texture's
    if (t.failed)
        return nullptr;
    if (!t.twin) {
        D3D11_TEXTURE2D_DESC td = d;
        td.Width = std::max(twin_w(d.Width), static_cast<unsigned>(focus_rect_in(d.Width, d.Height).right - focus_rect_in(d.Width, d.Height).left));
        td.Height = std::max(twin_h(d.Height), static_cast<unsigned>(focus_rect_in(d.Width, d.Height).bottom));
        for (UINT most = 1 + static_cast<UINT>(std::floor(std::log2(static_cast<double>(std::max(td.Width, td.Height)))));
             td.MipLevels > most;)
            td.MipLevels = most;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = d.MipLevels != 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = srgb_view(d.Format);
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g.device->CreateTexture2D(&td, nullptr, &t.twin)) ||
            FAILED(g.device->CreateRenderTargetView(t.twin.Get(), &rv, &t.twinRtv))) {
            t.twin.Reset();
            t.failed = true;
            simqvfr::logf("scs focus: no twin for a %ux%u texture of format %d (%d mips): its replays read the game's own",
                          d.Width, d.Height, static_cast<int>(d.Format), static_cast<int>(d.MipLevels));
            return nullptr;
        }
        if (td.MiscFlags)
            g.device->CreateShaderResourceView(t.twin.Get(), nullptr, &t.twinAll);
        t.mips = d.MipLevels;
        const float none[4] = {};
        g.context->ClearRenderTargetView(t.twinRtv.Get(), none);
        t.bytes = static_cast<uint64_t>(td.Width) * td.Height * 8;  // an upper estimate (16-bit RGBA)
        g.twinBytes += t.bytes;
        simqvfr::logf("scs focus: twin %zu: %ux%u for a %ux%u texture of format %d, %d mips", g.texes.size(), td.Width,
                      td.Height, d.Width, d.Height, static_cast<int>(d.Format), static_cast<int>(d.MipLevels));
    }
    const bool content = t.writeFrame != ~0ull && g.stats.frames - t.writeFrame <= 1;
    if (content && t.copiedSerial != t.writeSerial) {
        const D3D11_RECT f = focus_rect_in(d.Width, d.Height);
        const D3D11_BOX box = {static_cast<UINT>(f.left), 0, 0, static_cast<UINT>(f.right), static_cast<UINT>(f.bottom), 1};
        // First the eye's picture around the box (the AO reaches a quarter of a half-size texture past the region's
        // right and bottom edges; the other passes a few texels), then the focus region itself over its part.
        const float p = focus_fx();
        stretch_into_twin(t, game, d, ((d.Width * 2 <= g.eyeW + 2 ? 0.25f : 0.02f) + p) / p);
        g.context->CopySubresourceRegion(t.twin.Get(), 0, 0, 0, 0, game, 0, &box);
        if (t.twinAll)
            g.context->GenerateMips(t.twinAll.Get());
        t.copiedSerial = t.writeSerial;
        t.zero = false;
        ++g.twinCopies;
    } else if (!content) {
        if (stretch_into_twin(t, game, d)) {  // the eye's own, through the box (every time: the eye's changes)
            t.zero = false;
            t.copiedSerial = ~0ull;
        } else if (!t.zero) {
            const float none[4] = {};
            g.context->ClearRenderTargetView(t.twinRtv.Get(), none);
            t.zero = true;
            t.copiedSerial = ~0ull;
        }
    }
    return &t;
}

// The twin's view for a game view (the same format and mips).
ID3D11ShaderResourceView* twin_view(ID3D11ShaderResourceView* gameView, State::Tex& t) {
    State::TwinView& v = g.twinViews[gameView];
    v.usedFrame = g.stats.frames;
    if (!v.twin) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        gameView->GetDesc(&sd);
        if (FAILED(g.device->CreateShaderResourceView(t.twin.Get(), &sd, &v.twin)))
            return nullptr;
        v.game = gameView;  // kept, so the address isn't another view's later
    }
    return v.twin.Get();
}

// A full-screen pass the game just drew for the eye, drawn again for the focus view: its eye-shaped inputs from their
// twins, its constants rewritten for a region at the twins' origin with the focus camera, its output on the focus
// region of the game's own targets (with the game's depth and stencil there).
void replay(UINT vertices, UINT start) {
    constexpr UINT kSlots = 16;
    ID3D11ShaderResourceView* game[kSlots] = {};
    ID3D11ShaderResourceView* bind[kSlots] = {};
    g.context->PSGetShaderResources(0, kSlots, game);
    bool swapped = false;
    const bool record = g.burstFramesLeft > 0;
    char inputs[512] = "";
    for (UINT i = 0; i < kSlots; ++i) {
        bind[i] = game[i];
        if (!game[i])
            continue;
        ComPtr<ID3D11Resource> res;
        game[i]->GetResource(&res);
        ComPtr<ID3D11Texture2D> tex;
        if (!res || FAILED(res.As(&tex)))
            continue;
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        const bool shaped = d.SampleDesc.Count == 1 && d.ArraySize == 1 && eye_shaped(d.Width, d.Height);
        const char* how = "game's";
        if (shaped)
            if (State::Tex* t = twin_of(tex.Get(), d))
                if (ID3D11ShaderResourceView* v = twin_view(game[i], *t)) {
                    bind[i] = v;
                    swapped = true;
                    how = t->zero ? "twin, zero" : t->copiedSerial == ~0ull ? "twin, the eye's stretched" : "twin, focus";
                }
        if (record && g.stats.frames == g.burstStartFrame) {
            const size_t n = strlen(inputs);
            snprintf(inputs + n, sizeof(inputs) - n, " [%u: %ux%u format %d, %s]", i, d.Width, d.Height,
                     static_cast<int>(d.Format), how);
        }
    }
    if (swapped)
        g.context->PSSetShaderResources(0, kSlots, bind);
    rewrite(g.vsFocus, false, true);
    rewrite(g.psFocus, true, true);
    const D3D11_RECT f = focus_rect_in(g.boundW, g.boundH);
    const D3D11_VIEWPORT vp = {static_cast<float>(f.left), 0, static_cast<float>(f.right - f.left),
                               static_cast<float>(f.bottom), g.viewportCount ? g.viewports[0].MinDepth : 0,
                               g.viewportCount ? g.viewports[0].MaxDepth : 1};
    g.context->RSSetViewports(1, &vp);
    if (g.scissorCount)
        g.context->RSSetScissorRects(1, &f);
    g.context->Draw(vertices, start);
    if (record)
        shoot_stage(f, inputs);
    put_back(g.vsFocus, false);
    put_back(g.psFocus, true);
    if (swapped)
        g.context->PSSetShaderResources(0, kSlots, game);
    for (ID3D11ShaderResourceView* v : game)
        if (v)
            v->Release();
    g.viewsDual = false;
    apply_views();
    mark_bound_written();
    ++g.replays;
}

// NVAPI's two views on or off (the render thread). A refusal turns the focus view off for the session.
void two_views(bool on) {
    if (g.twoViews == on || !g.context)
        return;
    const int status = simqvfr::nvapi::set_two_views(g.context.Get(), on);
    if (status != 0 && on) {
        simqvfr::logf("scs focus: NVIDIA's two views refused (%s): the focus view's scene draws are off for this session",
                      simqvfr::nvapi::status_name(status));
        g.mvr = false;
        return;
    }
    g.twoViews = on;
}

// Two viewports and scissors: the game's own cut to the corner, and the focus view's.
void dual_views() {
    if (g.viewsDual || !g.viewportCount)
        return;
    const D3D11_RECT f = focus_rect();
    D3D11_VIEWPORT vp[2];
    scale_viewports(1, g.viewports, vp);
    vp[1] = {static_cast<float>(f.left), 0, static_cast<float>(f.right - f.left), static_cast<float>(f.bottom),
             vp[0].MinDepth, vp[0].MaxDepth};
    g.context->RSSetViewports(2, vp);
    if (g.scissorCount) {
        D3D11_RECT sc[2];
        scale_scissors(1, g.scissors, sc);
        sc[1] = f;
        g.context->RSSetScissorRects(2, sc);
    }
    g.viewsDual = true;
}

// An indexed draw of an eye pass's scene: the game shader's twin draws the eye and its focus view.
void focus_draw() {
    ID3D11VertexShader* vs = nullptr;
    g.context->VSGetShader(&vs, nullptr, nullptr);
    if (!vs)
        return;
    const bool instancing = g.instanced && (g.preferInstanced || !g.mvr);
    ID3D11VertexShader* twin = nullptr;
    UINT size = sizeof(twin);
    if (FAILED(vs->GetPrivateData(instancing ? lmu::kMvrInstancedTwinGuid : lmu::kMvrTwinGuid, &size, &twin)) || !twin) {
        vs->Release();
        ++g.noTwin;
        two_views(false);
        if (g.viewsDual) {  // this draw into the eye alone
            g.viewsDual = false;
            apply_views();
        }
        return;
    }
    two_views(!instancing);
    if (!instancing && !g.twoViews) {
        twin->Release();
        vs->Release();
        return;
    }
    g.instancedDraw = instancing;
    dual_views();
    if (const int eye = current_eye(); eye != g.remapEye) {  // this eye's box
        float r[4];
        box_remap(g.box[eye], r);
        g.context->UpdateSubresource(g.remapCb.Get(), 0, nullptr, r, 0, 0);
        g.remapEye = eye;
    }
    ID3D11Buffer* const remap[] = {g.remapCb.Get()};
    g.gameCb13.Reset();
    g.context->VSGetConstantBuffers(13, 1, g.gameCb13.GetAddressOf());  // put back after the draw, with the shader
    g.context->VSSetConstantBuffers(13, 1, remap);
    g.context->VSSetShader(twin, nullptr, 0);
    twin->Release();
    g.gameVs = vs;  // put back after the draw
    mark_bound_written();
    ++g.focusDraws;
}

// A draw whose shader focus_draw swapped for its instanced twin: drawn here with twice its instances, in the game's place.
bool on_draw_replace(ID3D11DeviceContext*, ch::Entry entry, const ch::DrawCall& d) {
    if (!g.instancedDraw)
        return false;
    g.instancedDraw = false;
    ID3D11DeviceContext1* c = g.context.Get();
    // Per-instance vertex data: the layout's twin, each step rate doubled, so a pair reads one instance's.
    ComPtr<ID3D11InputLayout> layout, twin;
    c->IAGetInputLayout(&layout);
    if (layout) {
        UINT size = sizeof(void*);
        IUnknown* stored = nullptr;  // SetPrivateDataInterface's: an IUnknown, asked for the layout (as LMU's doubled_draw)
        if (SUCCEEDED(layout->GetPrivateData(lmu::kMvrInstancedLayoutGuid, &size, &stored)) && stored) {
            stored->QueryInterface(IID_PPV_ARGS(twin.GetAddressOf()));
            stored->Release();
        }
        if (twin) {
            c->IASetInputLayout(twin.Get());
            ++g.doubledLayouts;
        }
    }
    switch (entry) {
        case ch::kDrawIndexed: c->DrawIndexedInstanced(d.count, 2, d.start, d.baseVertex, 0); break;
        case ch::kDrawIndexedInstanced:
            c->DrawIndexedInstanced(d.count, d.instances * 2, d.start, d.baseVertex, d.startInstance);
            break;
        case ch::kDraw: c->DrawInstanced(d.count, 2, d.start, 0); break;
        default: c->DrawInstanced(d.count, d.instances * 2, d.start, d.startInstance); break;
    }
    if (twin)
        c->IASetInputLayout(layout.Get());
    ++g.doubled;
    return true;
}

}  // namespace detail

}  // namespace scs
