// D3D11 quad-view compositor, for a D3D11 game whose one image holds all four views (ACC: OpenComposite copies
// the game's whole target into each eye's swapchain). Draws each eye's outer view upscaled over its tangents
// with its focus view blended into the focus box, both eyes side by side into a double-wide output: the same
// shader and constants as the D3D12 compositor (compose_shader.h), without the helmet.
//
// It records into a deferred context and executes that on the immediate one with the game's state restored,
// so it can run inside the game's own frame (xrEndFrame, on its render thread) without disturbing it.
#pragma once

#include <map>

#include <d3d11.h>
#include <wrl/client.h>

#include "compositor.h"

namespace aceqv {

class D3D11Compositor {
public:
    // viewFormat: the swapchains' colour format (the views read and write through it, so filtering happens in
    // linear space with an sRGB format); used for typeless images.
    bool initialize(ID3D11Device* device, DXGI_FORMAT viewFormat);
    bool ready() const { return ready_; }
    // Draws both eyes into `output` (double-wide: eye e into its half) from `views` (eyes[e].outerSource and
    // focusSource are regions of it). Both images stay bound to nothing afterwards.
    bool compose(ID3D11Texture2D* views, ID3D11Texture2D* output, const EyeComposition (&eyes)[2]);
    void shutdown();

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    ID3D11ShaderResourceView* source(ID3D11Texture2D* texture);
    ID3D11RenderTargetView* target(ID3D11Texture2D* texture);

    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> immediate_, deferred_;
    Com<ID3D11VertexShader> vs_;
    Com<ID3D11PixelShader> ps_;
    Com<ID3D11Buffer> constants_[2];  // one per eye
    Com<ID3D11SamplerState> sampler_;
    // Views of the swapchain images, made once each (the images live as long as their swapchains, and the
    // compositor is shut down with the session).
    std::map<ID3D11Texture2D*, Com<ID3D11ShaderResourceView>> sources_;
    std::map<ID3D11Texture2D*, Com<ID3D11RenderTargetView>> targets_;
    DXGI_FORMAT viewFormat_ = DXGI_FORMAT_UNKNOWN;
    bool ready_ = false;
};

}  // namespace aceqv
