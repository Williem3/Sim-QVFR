#pragma once

// RF2's frame census ([experiment] rf2_census=1, log only, off by default): for two sampled frames, every target bind on
// the observed context (its first target's size, format and samples, the depth view, the viewport, the scene-pass
// number and eye the pass state gives it, and the draws since the bind before), every resolve and command list. It is
// what the eye-pass rule (pass_state.h) is checked against on a headset, game settings and build other than the ones it
// was first measured on. Lines start with "rf2 census:".

#include <d3d11.h>
#include <wrl/client.h>

#include "../../log.h"

namespace simqvfr::games::rf2 {

class Census {
public:
    void set_on(bool on) { on_ = on; }
    bool on() const { return on_; }

    void frame_end(unsigned long long frame) {
        if (recording_)
            simqvfr::logf("rf2 census: frame %llu ends (%u draws after the last bind)", frame, draws_);
        recording_ = on_ && (frame == kFirst || frame == kSecond);
        draws_ = 0;
        if (recording_)
            simqvfr::logf("rf2 census: frame %llu begins", frame + 1);
    }

    void note_target(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                     unsigned pass, int eye, bool scene) {
        if (!recording_)
            return;
        D3D11_TEXTURE2D_DESC d{};
        describe(count ? rtvs[0] : nullptr, d);
        D3D11_VIEWPORT vp{};
        UINT n = 1;
        c->RSGetViewports(&n, &vp);
        simqvfr::logf("rf2 census: bind rts=%u rt0=%ux%u fmt=%u x%u dsv=%d vp=%.0f,%.0f %.0fx%.0f scene=%d pass=%u eye=%d "
                      "(draws before: %u)",
                      count, d.Width, d.Height, static_cast<unsigned>(d.Format), d.SampleDesc.Count, dsv ? 1 : 0,
                      n ? vp.TopLeftX : 0, n ? vp.TopLeftY : 0, n ? vp.Width : 0, n ? vp.Height : 0, scene ? 1 : 0, pass,
                      eye, draws_);
        draws_ = 0;
    }
    void note_draw() {
        if (recording_)
            ++draws_;
    }
    void note_resolve(ID3D11Resource* dst, ID3D11Resource* src, bool scene, int composedEye) {
        if (!recording_)
            return;
        D3D11_TEXTURE2D_DESC s{}, d{};
        describe(src, s);
        describe(dst, d);
        simqvfr::logf("rf2 census: resolve %ux%u fmt=%u x%u -> %ux%u fmt=%u scene=%d composed eye=%d (draws before: %u)",
                      s.Width, s.Height, static_cast<unsigned>(s.Format), s.SampleDesc.Count, d.Width, d.Height,
                      static_cast<unsigned>(d.Format), scene ? 1 : 0, composedEye, draws_);
        draws_ = 0;
    }
    void note_execute() {
        if (recording_)
            simqvfr::logf("rf2 census: command list (draws before: %u)", draws_);
    }

private:
    static constexpr unsigned long long kFirst = 600, kSecond = 1800;
    static void describe(ID3D11RenderTargetView* rtv, D3D11_TEXTURE2D_DESC& d) {
        if (!rtv)
            return;
        Microsoft::WRL::ComPtr<ID3D11Resource> r;
        rtv->GetResource(&r);
        describe(r.Get(), d);
    }
    static void describe(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& d) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> t;
        if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))))
            t->GetDesc(&d);
    }
    bool on_ = false, recording_ = false;
    unsigned draws_ = 0;
};

}  // namespace simqvfr::games::rf2
