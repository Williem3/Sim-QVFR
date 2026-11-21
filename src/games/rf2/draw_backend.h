#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "render_backend.h"
namespace simqvfr::games::rf2 {
struct DrawAtlas { D3D11_VIEWPORT viewport[2]{}; D3D11_RECT scissor[2]{}; };
inline DrawAtlas make_draw_atlas(const D3D11_VIEWPORT& v, const AtlasLayout& layout) {
    DrawAtlas a{}; a.viewport[0]=v; a.viewport[0].TopLeftX=0; a.viewport[0].TopLeftY=0;
    a.viewport[0].Width=static_cast<float>(layout.outerW); a.viewport[0].Height=static_cast<float>(layout.outerH);
    a.viewport[1]=a.viewport[0];
    a.viewport[1].TopLeftX=static_cast<float>(layout.focus_x());
    a.viewport[1].Width=static_cast<float>(layout.focusW); a.viewport[1].Height=static_cast<float>(layout.focusH);
    a.scissor[0]={0,0,static_cast<LONG>(layout.outerW),static_cast<LONG>(layout.outerH)};
    a.scissor[1]={static_cast<LONG>(layout.focus_x()),0,static_cast<LONG>(kSceneWidth),static_cast<LONG>(layout.focusH)};
    return a;
}
class RemapBuffer {
public:
    bool upload_and_bind(ID3D11DeviceContext* c, const float remap[4]) {
        if (!c || !ensure(c)) return false;
        c->UpdateSubresource(buffer_.Get(),0,nullptr,remap,0,0);
        c->VSSetConstantBuffers(13,1,buffer_.GetAddressOf()); return true;
    }
private:
    bool ensure(ID3D11DeviceContext* c) {
        Microsoft::WRL::ComPtr<ID3D11Device> d; c->GetDevice(&d); if(!d) return false;
        if(buffer_ && device_.Get()==d.Get()) return true;
        buffer_.Reset(); device_=d;
        const float identity[4]={1,0,1,0};
        const D3D11_BUFFER_DESC bd={16,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};
        const D3D11_SUBRESOURCE_DATA init={identity,0,0};
        return SUCCEEDED(d->CreateBuffer(&bd,&init,&buffer_));
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
};

inline Microsoft::WRL::ComPtr<ID3D11VertexShader> instanced_twin(
    ID3D11VertexShader* original, const GUID& privateDataGuid) {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> result;
    if (!original) return result;
    IUnknown* unknown = nullptr;
    UINT bytes = sizeof(unknown);
    if (SUCCEEDED(original->GetPrivateData(privateDataGuid, &bytes, &unknown)) && unknown) {
        unknown->QueryInterface(IID_PPV_ARGS(&result));
        unknown->Release();
    }
    return result;
}

class DrawState {
public:
    explicit DrawState(ID3D11DeviceContext* context) : context_(context) {
        if (!context_) return;
        instanceCount_ = 256;
        context_->VSGetShader(&shader_, instances_, &instanceCount_);
        viewportCount_ = 16;
        context_->RSGetViewports(&viewportCount_, viewports_);
        scissorCount_ = 16;
        context_->RSGetScissorRects(&scissorCount_, scissors_);
        context_->VSGetConstantBuffers(13, 1, &cb13_);
        valid_ = true;
    }
    ~DrawState() {
        for (UINT i = 0; i < instanceCount_; ++i)
            if (instances_[i]) instances_[i]->Release();
    }
    DrawState(const DrawState&) = delete;
    DrawState& operator=(const DrawState&) = delete;
    bool valid() const { return valid_; }
    ID3D11VertexShader* shader() const { return shader_.Get(); }
    ID3D11ClassInstance* const* instances() const { return instanceCount_ ? instances_ : nullptr; }
    UINT instance_count() const { return instanceCount_; }
    D3D11_VIEWPORT base_viewport() const {
        return viewportCount_ ? viewports_[0] : D3D11_VIEWPORT{0, 0, 3184, 3096, 0, 1};
    }
    template<class SetShader, class SetViewports, class SetScissors>
    void restore(SetShader&& setShader, SetViewports&& setViewports, SetScissors&& setScissors) const {
        if (!valid_) return;
        setShader(shader_.Get(), instances(), instanceCount_);
        ID3D11Buffer* cb = cb13_.Get();
        context_->VSSetConstantBuffers(13, 1, &cb);
        if (viewportCount_) setViewports(viewportCount_, viewports_);
        if (scissorCount_) setScissors(scissorCount_, scissors_);
    }
private:
    ID3D11DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> shader_;
    ID3D11ClassInstance* instances_[256]{};
    UINT instanceCount_ = 0;
    D3D11_VIEWPORT viewports_[16]{};
    UINT viewportCount_ = 0;
    D3D11_RECT scissors_[16]{};
    UINT scissorCount_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb13_;
    bool valid_ = false;
};

enum class DrawResult { handled, no_twin, backend_failed };

// `twinGuid`: which of the game shader's twins the draw uses (instanced, or NVIDIA's single-pass stereo). `prepare()`
// runs once the twin is known and before anything is changed (single-pass stereo: the two-view mode on); false: the
// draw is refused, nothing to restore. `draw()` makes the draw itself (twice the instances, or once).
template<class SetShader, class SetViewports, class SetScissors, class Draw, class Prepare>
DrawResult try_draw(ID3D11DeviceContext* context, const GUID& twinGuid, const float remap[4],
                    RemapBuffer& remapBuffer, const AtlasLayout& layout, SetShader&& setShader,
                    SetViewports&& setViewports, SetScissors&& setScissors, Draw&& draw, Prepare&& prepare) {
    DrawState state(context);
    if (!state.valid()) return DrawResult::backend_failed;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> twin = instanced_twin(state.shader(), twinGuid);
    if (!twin) return DrawResult::no_twin;
    if (!prepare()) return DrawResult::backend_failed;

    const DrawAtlas atlas = make_draw_atlas(state.base_viewport(), layout);
    setViewports(2, atlas.viewport);
    setScissors(2, atlas.scissor);
    setShader(twin.Get(), state.instances(), state.instance_count());

    if (!remapBuffer.upload_and_bind(context, remap)) {
        state.restore(setShader, setViewports, setScissors);
        return DrawResult::backend_failed;
    }

    draw();
    state.restore(setShader, setViewports, setScissors);
    return DrawResult::handled;
}

}
