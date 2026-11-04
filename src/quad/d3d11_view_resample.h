// Fills a focus view's region of a texture from its eye's region, through the focus camera's geometry: each
// focus texel takes the eye's value in the same direction (the focus view's tangents, its turn in the eye's
// frame, the eye's tangents). For a D3D11 game whose screen-space effects are computed per view, so the focus
// view can reuse its eye's (ACC's reflections and ambient occlusion: src/games/acc/view_share.h).
//
// Two paths: `resample`, a compute shader writing the destination through a UAV (for compute outputs, as SSR's
// denoiser writes); `resample_target`, a draw into a render target (for draws' outputs, as ACC's AO), which may
// read its own destination's texture (the eye's region is copied aside first). Both are recorded into a deferred
// context and executed on the immediate one with the game's state restored, so they can run between two of the
// game's own draws.
#pragma once

#include <map>

#include <d3d11.h>
#include <wrl/client.h>

namespace aceqv {

struct ViewRegion {
    int x = 0, y = 0, width = 0, height = 0;  // texels of the texture
};

struct ViewMapping {
    float focusTan[4];  // the destination view's tangents: left, right, up, down
    float eyeTan[4];    // the source view's
    float rotation[9];  // row-major: a direction in the focus view's frame into the eye's
};

class D3D11ViewResample {
public:
    bool initialize(ID3D11Device* device);
    bool ready() const { return ready_; }
    // `dst`'s `dstRegion` from `src`'s `srcRegion`, bilinear, clamped to `srcRegion`. Float or unorm textures of
    // the device it was made on: `src` a shader resource, `dst` an unordered access view (another texture).
    bool resample(ID3D11Texture2D* src, const ViewRegion& srcRegion, ID3D11Texture2D* dst, const ViewRegion& dstRegion,
                  const ViewMapping& mapping);
    // The same into a render target; `src` may be `dst` (regions apart).
    bool resample_target(ID3D11Texture2D* src, const ViewRegion& srcRegion, ID3D11Texture2D* dst,
                         const ViewRegion& dstRegion, const ViewMapping& mapping);
    // Drops the views it keeps of textures it was given (they keep those textures alive).
    void forget_views();
    void shutdown();

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    ID3D11ShaderResourceView* source(ID3D11Texture2D* texture);
    ID3D11UnorderedAccessView* target(ID3D11Texture2D* texture);
    ID3D11RenderTargetView* render_target(ID3D11Texture2D* texture);
    ID3D11Texture2D* scratch(ID3D11Texture2D* src, const ViewRegion& region);
    bool constants(const ViewMapping& m, const ViewRegion& srcRegion, const ViewRegion& dstRegion, UINT srcWidth,
                   UINT srcHeight);
    bool execute();

    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> immediate_, deferred_;
    Com<ID3D11ComputeShader> cs_;
    Com<ID3D11VertexShader> vs_;
    Com<ID3D11PixelShader> ps_;
    Com<ID3D11RasterizerState> raster_;
    Com<ID3D11Buffer> constants_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11Texture2D> scratch_;
    Com<ID3D11ShaderResourceView> scratchView_;
    std::map<ID3D11Texture2D*, Com<ID3D11ShaderResourceView>> sources_;
    std::map<ID3D11Texture2D*, Com<ID3D11UnorderedAccessView>> targets_;
    std::map<ID3D11Texture2D*, Com<ID3D11RenderTargetView>> renderTargets_;
    bool ready_ = false;
};

}  // namespace aceqv
