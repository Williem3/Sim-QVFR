#include "adapter.h"

#include <windows.h>
#include <d3d11_4.h>

#include <cmath>

#include "../../engine/context_hooks.h"
#include "../../engine/nvapi_stereo.h"
#include "../../log.h"
#include "../../quad/regulator_settings.h"
#include "../identity_adapter.h"
#include "../lmu/mvr_probe.h"
#include "game.h"
#include "renderer.h"

// rFactor 2's QVFR at the D3D11 level (renderer.h): no game code is hooked or called. The layer runs as mode=off
// (GameAdapter::adapter_qvfr) and hands this adapter the ini's sizes, focus box and look, the regulator's settings, the
// offered eye size and the FOV; the engine's context hooks (engine/context_hooks.h) feed the renderer, LMU's shader probe
// gives the twins (as ATS and ETS2 use it).

namespace rf2 {
namespace {

namespace ch = simqvfr::context_hooks;
using simqvfr::games::rf2::MultiView;
using simqvfr::games::rf2::Renderer;

Renderer renderer;

// The context hooks' observers: straight to the renderer (the game's render thread).
void on_targets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    renderer.targets(c, n, rtvs, dsv);
}
void on_draw(ID3D11DeviceContext* c, ch::Entry entry, UINT, UINT) { renderer.draw(c, entry); }
bool on_draw_replace(ID3D11DeviceContext* c, ch::Entry entry, const ch::DrawCall& d) {
    return renderer.draw_replace(c, entry, d);
}
void on_draw_after(ID3D11DeviceContext* c, ch::Entry, UINT, UINT, UINT) { renderer.draw_after(c); }
void on_execute(ID3D11DeviceContext* c, ID3D11CommandList*) { renderer.execute(c); }
void on_resolve(ID3D11DeviceContext* c, ID3D11Resource*, UINT, ID3D11Resource* src, UINT, DXGI_FORMAT) {
    renderer.resolve(c, src);
}
void on_resolved(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT, DXGI_FORMAT format) {
    renderer.resolved(c, dst, dstSub, src, format);
}
void on_other_targets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    renderer.other_targets(c, n, rtvs, dsv);
}

class Rf2Adapter final : public simqvfr::games::IdentityAdapter {
public:
    Rf2Adapter() : IdentityAdapter(&kGame) {}
    bool has_qvfr() const override { return true; }
    bool adapter_qvfr() const override { return true; }

    // The ini's focus box (at the start and live): the share of the eye's field of view the focus view covers.
    void qvfr_set_live(double focusFraction, double focusFractionY, double, bool) override {
        if (analysis())
            renderer.set_focus(focusFraction, focusFractionY);
    }
    // The ini's view sizes (periphery_scale, focus density): the outer and focus views' pixel counts in the scene target.
    void qvfr_set_density(double peripheryScale, double focusWidth, double focusHeight) override {
        if (analysis())
            renderer.set_density(peripheryScale, focusWidth, focusHeight);
    }
    // The ini's [regulator] (at the start and live), and the set focus density it works down from.
    void qvfr_set_regulator(const simqvfr::GpuRegulatorSettings& s, double density) override {
        if (analysis())
            renderer.set_regulator(s, density);
    }
    simqvfr::games::QvfrRegulatorStatus qvfr_regulator_status() const override {
        simqvfr::games::QvfrRegulatorStatus r;
        if (analysis())
            r.active = renderer.regulator_status(r.focus, r.periphery, r.peripheryPercent, r.densityPercent, r.cutPeriphery,
                                                 r.cutDensity);
        return r;
    }
    // The ini's feather, corner radius, outline and the focus box's vertical offset (at the start and live).
    void qvfr_set_focus_look(double feather, double cornerRadius, bool outline, const float (&outlineColor)[4],
                             double offsetDegrees) override {
        if (analysis())
            renderer.set_focus_look(feather, cornerRadius, outline, outlineColor, offsetDegrees);
    }
    void views_offered(uint32_t width, uint32_t height) override { renderer.set_eye_size(width, height); }

    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        if (!analysis())
            return;
        shaderProbe_ = lmu::mvr_probe_install(ini, true);  // the game shaders' NVAPI and instanced twins
        simqvfr::logf("rf2: shader twins %s", shaderProbe_ ? "on" : "unavailable: QVFR off");
        renderer.set_gpu_timing(ini ? static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gpu_timing", 0, ini)) : 0);
        renderer.set_census(ini && GetPrivateProfileIntW(L"experiment", L"rf2_census", 0, ini) != 0);
    }
    void d3d11_session(ID3D11Device* device) override {
        if (!analysis() || !device || !shaderProbe_)
            return;
        lmu::mvr_probe_device(device);
        // NVIDIA's multi-view rendering where the device has it (and multi_view_software=1 isn't set), else instancing
        // where a vertex shader may pick the viewport; neither: the game draws as without Sim-QVFR.
        const simqvfr::nvapi::Support support = simqvfr::nvapi::query(device);
        D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
        const bool instancing = lmu::mvr_probe_instanced() &&
                                SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3))) &&
                                o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
        const bool nvidia = support.xyzw && lmu::mvr_probe_backend() == lmu::MvrBackend::nvapi;
        renderer.set_backend(nvidia ? MultiView::nvidia : instancing ? MultiView::instancing : MultiView::none, instancing);
        if (!nvidia && !instancing)
            return;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        ch::Observers o;
        o.targets = &on_targets;
        o.draw = &on_draw;
        o.draw_replace = &on_draw_replace;
        o.draw_after = &on_draw_after;
        o.execute = &on_execute;
        o.resolve = &on_resolve;
        o.resolved = &on_resolved;
        o.other_targets = &on_other_targets;
        const uint32_t wanted = (1u << ch::kOMSetRenderTargets) | (1u << ch::kOMSetRenderTargetsAndUavs) |
                                (1u << ch::kDrawIndexed) | (1u << ch::kDraw) | (1u << ch::kDrawIndexedInstanced) |
                                (1u << ch::kDrawInstanced) | (1u << ch::kDrawIndexedInstancedIndirect) |
                                (1u << ch::kDrawInstancedIndirect) | (1u << ch::kExecuteCommandList) |
                                (1u << ch::kResolveSubresource);
        hooked_ = context && ch::install(context.Get(), wanted, o);
        simqvfr::logf("rf2: D3D11 hooks %s (session device %p)", hooked_ ? "on" : "FAILED: QVFR off",
                      static_cast<void*>(device));
    }
    void frame_end() override {
        if (shaderProbe_)
            lmu::mvr_probe_frame_end();
        if (!hooked_)
            return;
        renderer.end_frame();
        ch::rescan();
    }
    void session_destroyed() override {
        if (hooked_)
            ch::uninstall();
        hooked_ = false;
        IdentityAdapter::session_destroyed();
    }
    void qvfr_note_views(const XrView* views, uint32_t count) override {
        if (!analysis() || !views || count < 2)
            return;
        float tan[2][4];
        for (int eye = 0; eye < 2; ++eye) {
            tan[eye][0] = std::tan(views[eye].fov.angleLeft);
            tan[eye][1] = std::tan(views[eye].fov.angleRight);
            tan[eye][2] = std::tan(views[eye].fov.angleDown);
            tan[eye][3] = std::tan(views[eye].fov.angleUp);
            if (!(tan[eye][1] > tan[eye][0]) || !(tan[eye][3] > tan[eye][2]))
                return;
        }
        renderer.note_fov(tan);
    }

private:
    bool shaderProbe_ = false, hooked_ = false;
};

Rf2Adapter g_adapter;

}  // namespace

simqvfr::games::GameAdapter& adapter() { return g_adapter; }

}  // namespace rf2
