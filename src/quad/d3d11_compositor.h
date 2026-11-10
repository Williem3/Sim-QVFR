// D3D11 quad-view compositor, for a D3D11 game whose one image holds all four views (ACC: OpenComposite copies
// the game's whole target into each eye's swapchain). Draws each eye's outer view upscaled over its tangents
// with its focus view blended into the focus box, both eyes side by side into a double-wide output: the same
// shader and constants as the D3D12 compositor (compose_shader.h), the helmet overlay included (its light,
// built-in look, pieces and its own layer, helmet_pieces.h).
//
// It records into a deferred context and executes that on the immediate one with the game's state restored,
// so it can run inside the game's own frame (xrEndFrame, on its render thread) without disturbing it.
#pragma once

#include <map>
#include <utility>

#include <d3d11.h>
#include <wrl/client.h>

#include "compose_shader.h"
#include "compositor.h"
#include "helmet_pieces.h"

namespace simqvfr {

class D3D11Compositor : public HelmetPieceSet {
public:
    // viewFormat: the swapchains' colour format (the views read and write through it, so filtering happens in
    // linear space with an sRGB format); used for typeless images.
    bool initialize(ID3D11Device* device, DXGI_FORMAT viewFormat);
    bool ready() const { return ready_; }
    // Draws both eyes into `output` (double-wide: eye e into its half) from `views` (eyes[e].outerSource and
    // focusSource are regions of it). With the helmet in either eye (EyeComposition::helmet), the frame's light
    // is taken from the eyes' outer views first. Both images stay bound to nothing afterwards.
    bool compose(ID3D11Texture2D* views, ID3D11Texture2D* output, const EyeComposition (&eyes)[2]);
    // Draws the helmet's own layer into `target` (a kHelmetLayerFormat swapchain image): the frame's light from
    // `views` (d.outerSource), then the pieces, premultiplied, with alpha = how much of the scene they hide; only
    // d.bands (all of it when empty). False without pieces.
    bool draw_helmet_layer(ID3D11Texture2D* views, ID3D11Texture2D* target, const HelmetLayerDraw& d);
    // focus_layer: draws each eye's focus view into `target` (a kHelmetLayerFormat swapchain image), eye e at `rects[e]`
    // (x, y, width, height in pixels: the focus view's own size, so its region of `views` is copied texel for texel),
    // premultiplied, alpha its feather (FocusLayerPS).
    bool draw_focus_layer(ID3D11Texture2D* views, ID3D11Texture2D* target, const EyeComposition (&eyes)[2],
                          const int (&rects)[2][4]);
    // Sharpening without composition (focus_layer, native quad views): each eye's view `sources[e]` (a region of
    // `views`, u0, v0, du, dv) copied texel for texel into `target` (the views' format) at `rects[e]` (as
    // draw_focus_layer's), sharpened by `sharpness` (EyeComposition's; ViewCopyPS).
    bool draw_view_copies(ID3D11Texture2D* views, ID3D11Texture2D* target, const float (&sources)[2][4],
                          const int (&rects)[2][4], float sharpness);
    void request_light() override { lightWanted_ = true; }
    bool read_light(float (&rgb)[2][4][3]) override;
    void shutdown();

protected:
    bool pieces_ready() const override { return ready_; }
    bool upload_piece(int piece, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) override;
    void drop_piece(int piece) override { pieces_[piece].Reset(); }

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    ID3D11ShaderResourceView* source(ID3D11Texture2D* texture);
    // A render target view of `texture`, as `typelessAs` when the texture is typeless.
    ID3D11RenderTargetView* target(ID3D11Texture2D* texture, DXGI_FORMAT typelessAs);
    bool set_constants(int buffer, const float (&constants)[kComposeConstants]);
    // Records the light pass (and read_light's copy) into the deferred context: each eye's sides from `views`.
    bool record_light(ID3D11ShaderResourceView* views, const float (&source0)[4], const float (&source1)[4]);
    // t0 and t1 `views`, t2 the light, t3-t5 the pieces (null where none).
    void bind_sources(ID3D11ShaderResourceView* views);
    bool finish();  // executes the deferred context on the immediate one, the game's state as it was
    bool discard();  // drops what the deferred context recorded; false

    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> immediate_, deferred_;
    Com<ID3D11VertexShader> vs_;
    Com<ID3D11PixelShader> ps_, lightPs_, helmetPs_, focusPs_, copyPs_;
    static constexpr int kLightBuffer = 2, kLayerBuffer = 3;
    Com<ID3D11Buffer> constants_[4];  // one per eye, the light pass, the helmet layer
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11BlendState> lightBlend_;      // the light pass: blended with the last frames' by the blend factor
    Com<ID3D11RasterizerState> scissored_;  // the helmet layer: only its bands
    // The helmet's light: 4 x 2 (left, right, top, bottom per eye), and read_light's copy of it.
    Com<ID3D11Texture2D> light_, lightReadback_;
    Com<ID3D11ShaderResourceView> lightSrv_;
    Com<ID3D11RenderTargetView> lightRtv_;
    bool lightFresh_ = true, lightWanted_ = false, lightCopied_ = false;
    Com<ID3D11ShaderResourceView> pieces_[3];  // HelmetPiece order
    // Views of the swapchain images, made once each (the images live as long as their swapchains, and the
    // compositor is shut down with the session).
    std::map<ID3D11Texture2D*, Com<ID3D11ShaderResourceView>> sources_;
    std::map<std::pair<ID3D11Texture2D*, DXGI_FORMAT>, Com<ID3D11RenderTargetView>> targets_;
    DXGI_FORMAT viewFormat_ = DXGI_FORMAT_UNKNOWN;
    bool ready_ = false;
};

}  // namespace simqvfr
