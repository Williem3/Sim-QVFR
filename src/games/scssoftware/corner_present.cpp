// The headset images: each eye's corner scaled back over its image at release, the focus view composed over its
// box, and the OpenXR swapchain calls (corner_internal.h).
#include "corner_internal.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>

#include "../../log.h"

namespace scs {
namespace detail {

constexpr char kBlit[] = R"(
Texture2D src : register(t0);
SamplerState s : register(s0);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(O i) : SV_Target { return src.SampleLevel(s, i.uv, 0); }
// A float target for a burst's 8-bit shot: x / (1 + x), so values over 1 keep their differences.
float4 squash(O i) : SV_Target {
    float3 c = max(src.SampleLevel(s, i.uv, 0).rgb, 0);
    return float4(c / (1 + c), 1);
}
// The focus view over the box: a rounded box, its edge blended into the eye's picture, an outline if asked for.
// `look`: the blend's width and the corner radius in units of half the box's shorter side, outline on, the box's
// width over its height in pixels.
cbuffer Look : register(b1) { float4 look; float4 lookColor; };
float4 feather(O i) : SV_Target {
    float2 half = look.w >= 1 ? float2(look.w, 1) : float2(1, 1 / look.w);
    float2 q = abs(i.uv * 2 - 1) * half;
    float r = saturate(look.y);
    float2 dd = q - (half - r);
    float inside = r - (length(max(dd, 0)) + min(max(dd.x, dd.y), 0));  // from the edge inwards
    float3 c = src.SampleLevel(s, i.uv, 0).rgb;
    if (look.z > 0.5 && inside < 0.012)
        return float4(lookColor.rgb, inside >= 0 ? 1 : 0);
    return float4(c, saturate(inside / max(look.x, 0.002)));
}
)";

DXGI_FORMAT srgb_view(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return f;
    }
}

bool blit_resources() {
    if (g.blitPs)
        return true;
    ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(D3DCompile(kBlit, sizeof(kBlit) - 1, "scs-corner-blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors)) ||
        FAILED(D3DCompile(kBlit, sizeof(kBlit) - 1, "scs-corner-blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors)))
        return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3DBlob> fps, sps;
    if (SUCCEEDED(D3DCompile(kBlit, sizeof(kBlit) - 1, "scs-corner-blit", nullptr, nullptr, "squash", "ps_5_0", 0, 0, &sps, &errors)))
        g.device->CreatePixelShader(sps->GetBufferPointer(), sps->GetBufferSize(), nullptr, &g.squashPs);
    if (FAILED(D3DCompile(kBlit, sizeof(kBlit) - 1, "scs-corner-blit", nullptr, nullptr, "feather", "ps_5_0", 0, 0, &fps, &errors)))
        return false;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                          D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_COLOR_WRITE_ENABLE_ALL};
    return SUCCEEDED(g.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.blitVs)) &&
           SUCCEEDED(g.device->CreatePixelShader(fps->GetBufferPointer(), fps->GetBufferSize(), nullptr, &g.featherPs)) &&
           SUCCEEDED(g.device->CreateBlendState(&bd, &g.featherBlend)) &&
           SUCCEEDED(g.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.blitPs)) &&
           SUCCEEDED(g.device->CreateSamplerState(&sd, &g.linear)) &&
           SUCCEEDED(g.device->CreateRasterizerState(&rd, &g.raster));
}

ID3D11RenderTargetView* target_view(ID3D11Texture2D* image, const D3D11_TEXTURE2D_DESC& d) {
    for (State::Target& t : g.targets)
        if (t.image == image)
            return t.view.Get();
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = srgb_view(d.Format);
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    State::Target t;
    t.image = image;
    if (FAILED(g.device->CreateRenderTargetView(image, &rv, &t.view)))
        return nullptr;
    g.targets.push_back(t);
    return g.targets.back().view.Get();
}

// The corner of `image` (cw x ch) drawn back over the whole image; the game's state put back. False: nothing changed.
bool upscale(ID3D11Texture2D* image, UINT cw, UINT ch, int eye) {
    D3D11_TEXTURE2D_DESC d;
    image->GetDesc(&d);
    if (!blit_resources() || d.SampleDesc.Count != 1 || !(d.BindFlags & D3D11_BIND_RENDER_TARGET) || cw > d.Width ||
        ch > d.Height)
        return false;
    if (!g.scratch || g.scratchDesc.Width != cw || g.scratchDesc.Height != ch || g.scratchDesc.Format != d.Format) {
        g.scratch.Reset();
        g.scratchView.Reset();
        D3D11_TEXTURE2D_DESC sd = {cw, ch, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = srgb_view(d.Format);
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &g.scratch)) ||
            FAILED(g.device->CreateShaderResourceView(g.scratch.Get(), &sv, &g.scratchView)))
            return false;
        g.scratchDesc = sd;
    }
    ID3D11RenderTargetView* rtv = target_view(image, d);
    if (!rtv)
        return false;
    // The focus view, if this image's focus region was drawn this frame: kept aside before the eye covers it.
    const D3D11_RECT fr = focus_rect_in(d.Width, d.Height);
    const UINT fw = static_cast<UINT>(fr.right - fr.left), fh = static_cast<UINT>(fr.bottom);
    bool focus = false;
    if (g.focusPercent && g.composeFocus && fw && fh) {
        const auto it = g.texes.find(image);
        focus = it != g.texes.end() && it->second.writeFrame == g.stats.frames;
    }
    if (focus && (!g.focusScratch || g.focusScratchDesc.Width != fw || g.focusScratchDesc.Height != fh ||
                  g.focusScratchDesc.Format != d.Format)) {
        g.focusScratch.Reset();
        g.focusScratchView.Reset();
        D3D11_TEXTURE2D_DESC sd = {fw, fh, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = srgb_view(d.Format);
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &g.focusScratch)) ||
            FAILED(g.device->CreateShaderResourceView(g.focusScratch.Get(), &sv, &g.focusScratchView)))
            focus = false;
        else
            g.focusScratchDesc = sd;
    }
    two_views(false);
    ID3D11DeviceContext1* c = g.context.Get();
    const ch::Bypass bypass;
    // The game's state this touches (no scissors: none set), put back when this returns, inside the bypass.
    const simqvfr::D3D11StateGuard keep(c, blit_state(d3s::kInputLayout | d3s::kVS | d3s::kPS | d3s::kGsHsDs |
                                                      d3s::kOMTargets | d3s::kBlend | d3s::kDepth | d3s::kRSState |
                                                      d3s::kViewports));

    const D3D11_BOX box = {0, 0, 0, cw, ch, 1};
    c->CopySubresourceRegion(g.scratch.Get(), 0, 0, 0, 0, image, 0, &box);
    if (focus) {
        const D3D11_BOX fbox = {static_cast<UINT>(fr.left), 0, 0, static_cast<UINT>(fr.right), fh, 1};
        c->CopySubresourceRegion(g.focusScratch.Get(), 0, 0, 0, 0, image, 0, &fbox);
    }
    ID3D11ShaderResourceView* noSrv[] = {nullptr};
    c->PSSetShaderResources(0, 1, noSrv);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT full = {0, 0, static_cast<float>(d.Width), static_cast<float>(d.Height), 0, 1};
    c->RSSetViewports(1, &full);
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
    ID3D11ShaderResourceView* src[] = {g.scratchView.Get()};
    ID3D11SamplerState* s[] = {g.linear.Get()};
    c->PSSetShaderResources(0, 1, src);
    c->PSSetSamplers(0, 1, s);
    c->Draw(3, 0);
    if (focus) {  // over the box: the focus region stretched (or shrunk) to its share of the image
        const float* b = g.box[std::min(std::max(eye, 0), 1)];
        const D3D11_VIEWPORT bv = {b[0] * d.Width, b[1] * d.Height, beta_x() * d.Width, beta_y() * d.Height, 0, 1};
        c->RSSetViewports(1, &bv);
        ID3D11ShaderResourceView* fsrc[] = {g.focusScratchView.Get()};
        c->PSSetShaderResources(0, 1, fsrc);
        c->PSSetShader(g.featherPs.Get(), nullptr, 0);
        c->OMSetBlendState(g.featherBlend.Get(), nullptr, 0xffffffff);
        // The edge: the game's own buffer in pixel slot 1 kept and put back after the draw, its range with it.
        d3s::Spec cbSpec;
        cbSpec.psCb = {1, 1};
        const simqvfr::D3D11StateGuard keepCb(c, cbSpec);
        if (g.lookCb) {
            const float look[8] = {std::max(0.0f, g.lookFeather.load(std::memory_order_relaxed)),
                                   g.lookRadius.load(std::memory_order_relaxed),
                                   g.lookBorder.load(std::memory_order_relaxed) ? 1.0f : 0.0f,
                                   bv.Height > 0 ? bv.Width / bv.Height : 1.0f,
                                   g.lookColor[0].load(std::memory_order_relaxed), g.lookColor[1].load(std::memory_order_relaxed),
                                   g.lookColor[2].load(std::memory_order_relaxed), 1};
            c->UpdateSubresource(g.lookCb.Get(), 0, nullptr, look, 0, 0);
            ID3D11Buffer* const cb[] = {g.lookCb.Get()};
            c->PSSetConstantBuffers(1, 1, cb);
        }
        c->Draw(3, 0);
        ++g.composed;
    }

    c->PSSetShaderResources(0, 1, noSrv);
    return true;
}

State::Chain* chain_of(XrSwapchain handle, PFN_xrEnumerateSwapchainImages enumerate) {
    for (State::Chain& c : g.chains)
        if (c.handle == handle)
            return &c;
    if (!enumerate)
        return nullptr;
    uint32_t count = 0;
    if (XR_FAILED(enumerate(handle, 0, &count, nullptr)) || !count)
        return nullptr;
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(enumerate(handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))))
        return nullptr;
    State::Chain c;
    c.handle = handle;
    for (const auto& i : images)
        c.images.push_back(i.texture);
    g.chains.push_back(c);
    simqvfr::logf("scs corner: swapchain %p: %u images known (the corner is scaled back over them at xrEndFrame)",
                  static_cast<void*>(handle), count);
    return &g.chains.back();
}

}  // namespace detail

using namespace detail;

const XrFrameEndInfo* corner_frame_end(const XrFrameEndInfo* info, PFN_xrEnumerateSwapchainImages enumerate) {
    // First the session start or end the main thread asked for: here, on the thread that ends the frames, between its
    // commands (corner.cpp, service_lifecycle). A frame of a session being set up goes out as drawn.
    if (g.lifecyclePending.load(std::memory_order_acquire))
        service_lifecycle();
    if (!g.context)
        return info;
    device_watch();
    g.endFrames.fetch_add(1, std::memory_order_release);
    // The context's entries hooked again if the game's mode changed them: on its own thread, between its commands
    // (context_hooks.h), never inside an observer.
    if (g.thread && GetCurrentThreadId() == g.thread)
        ch::rescan();
    // The frame being submitted: whether its eyes went into the corner (ATS draws a frame, then ends it).
    const bool cornered = g.cornerNow.exchange(false, std::memory_order_acq_rel);
    if (g.burstFramesLeft > 0 && g.thread && GetCurrentThreadId() == g.thread && --g.burstFramesLeft == 0)
        write_burst();
    if (!cornered || !info)
        return info;
    std::lock_guard lock(g.frameMutex);
    g.projections.clear();
    g.views.clear();
    g.layers.assign(info->layers, info->layers + info->layerCount);
    size_t viewTotal = 0;
    for (uint32_t i = 0; i < info->layerCount; ++i)
        if (info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            viewTotal += reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i])->viewCount;
    g.projections.reserve(info->layerCount);
    g.views.reserve(viewTotal);
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        const size_t at = g.views.size();
        for (uint32_t v = 0; v < proj->viewCount; ++v) {
            XrCompositionLayerProjectionView view = proj->views[v];
            XrRect2Di& r = view.subImage.imageRect;
            const UINT cw = static_cast<UINT>(std::max(1, static_cast<int32_t>(std::lround(r.extent.width * g.fx))));
            const UINT ch = static_cast<UINT>(std::max(1, static_cast<int32_t>(std::lround(r.extent.height * g.fy))));
            State::Chain* chain = chain_of(view.subImage.swapchain, enumerate);
            if (chain && chain->scaled && r.offset.x == 0 && r.offset.y == 0 && view.subImage.imageArrayIndex == 0) {
                ++g.upscaled;  // scaled back at its release: its rectangle as the game gave it
            } else {
                ++g.upscaleMisses;
                r.extent.width = static_cast<int32_t>(cw);
                r.extent.height = static_cast<int32_t>(ch);
            }
            if (chain) {
                chain->scaled = false;
                chain->eye = static_cast<int>(v);
            }
            g.views.push_back(view);
        }
        g.projections.push_back(*proj);
        g.projections.back().views = g.views.data() + at;
        g.layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&g.projections.back());
    }
    g.info = *info;
    g.info.layers = g.layers.data();
    return &g.info;
}

void corner_acquired(XrSwapchain chain, uint32_t index, PFN_xrEnumerateSwapchainImages enumerate) {
    if (!g.context || g.lifecyclePending.load(std::memory_order_acquire))
        return;  // off, or a session's start or end still to be done: the state is the old session's
    std::lock_guard lock(g.frameMutex);
    if (State::Chain* c = chain_of(chain, enumerate)) {
        c->held.push_back(index);
        const int eye = c->eye >= 0 ? c->eye : static_cast<int>((c - g.chains.data()) & 1);
        if (eye == 0)  // the key of the frame's eye order (scene_eye)
            g.acquiredIndex.store(static_cast<int>(index), std::memory_order_release);
        if (c->held.size() > 1 && g.heldTwo++ == 0)
            simqvfr::logf("scs corner: the game acquires its next swapchain image before releasing the last one (%zu held): "
                          "each release scales the oldest", c->held.size());
    }
}

void corner_releasing(XrSwapchain chain) {
    if (!g.context || g.lifecyclePending.load(std::memory_order_acquire))
        return;
    // The image this release hands back: the oldest the game holds (taken under the lock: corner_acquired may grow the
    // list from another thread meanwhile).
    int chainIdx = -1;
    uint32_t released = ~0u;
    ID3D11Texture2D* image = nullptr;
    {
        std::lock_guard lock(g.frameMutex);
        for (size_t i = 0; i < g.chains.size(); ++i)
            if (g.chains[i].handle == chain && !g.chains[i].held.empty()) {
                chainIdx = static_cast<int>(i);
                released = g.chains[i].held.front();
                g.chains[i].held.pop_front();
                if (released < g.chains[i].images.size())
                    image = g.chains[i].images[released];
                break;
            }
    }
    if (!image)
        return;
    // The drawing thread's own state only on it; elsewhere the published copy.
    const bool onThread = g.thread && GetCurrentThreadId() == g.thread;
    const bool active = onThread ? g.active : g.activeNow.load(std::memory_order_relaxed);
    if (onThread)
        poll_diag_hotkeys();
    const bool burst = onThread && g.burstFramesLeft > 0;
    if (burst && !active) {  // a frame without its eyes in the corner, recorded too
        std::lock_guard lock(g.frameMutex);
        shoot(image, chainIdx, released, true, false);
    }
    if (!active)
        return;  // a frame without its eyes in the corner: the image as the game drew it
    if (!onThread) {
        if (g.otherThreadReleases++ == 0)
            simqvfr::log_line("scs corner: a swapchain image released off the render thread: not scaled (the corner is cut "
                              "at xrEndFrame instead)");
        return;
    }
    std::lock_guard lock(g.frameMutex);
    State::Chain* found = nullptr;  // found again: the list may have changed since
    for (State::Chain& x : g.chains)
        if (x.handle == chain)
            found = &x;
    if (!found)
        return;
    State::Chain& c = *found;
    D3D11_TEXTURE2D_DESC d;
    image->GetDesc(&d);
    const UINT cw = static_cast<UINT>(std::max(1L, std::lround(d.Width * g.fx)));
    const UINT ch = static_cast<UINT>(std::max(1L, std::lround(d.Height * g.fy)));
    if (burst)
        shoot(image, chainIdx, released, false, false);
    {
        const Timed t(State::kTimeRelease);
        c.scaled = upscale(image, cw, ch, c.eye >= 0 ? c.eye : chainIdx & 1);
    }
    if (burst)
        shoot(image, chainIdx, released, true, c.scaled);
}

void corner_swapchain_destroyed(XrSwapchain chain) {
    // Every use of g.chains (and g.targets' views of their images) is under frameMutex, on whichever thread, so the
    // entry goes at once; nothing waits for the session lifecycle (release_all clears them all anyway).
    std::lock_guard lock(g.frameMutex);
    for (auto it = g.chains.begin(); it != g.chains.end();) {
        if (it->handle != chain) {
            ++it;
            continue;
        }
        std::erase_if(g.targets, [&](const State::Target& t) {
            return std::find(it->images.begin(), it->images.end(), t.image) != it->images.end();
        });
        simqvfr::logf("scs corner: swapchain %p destroyed: its %zu images forgotten", static_cast<void*>(chain),
                      it->images.size());
        it = g.chains.erase(it);
    }
}

}  // namespace scs
