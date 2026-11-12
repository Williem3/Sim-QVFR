// LMU MVR's focus views post-processed in their eye's pass (docs/recon-lmu.md "Step 3, revised"). MVR draws each focus
// view beside its eye; YEBIS post-processes only the eye's part of the image. Right after the eye's own YEBIS draws, this
// runs the two that make the focus view's picture once more, on its part: YEBIS's copy of the resolved scene and its
// composite (scene, glare, the 1x1 exposure: tone curve, colour matrix, gamma; per pixel), with the game's own pixel
// shaders, constants and samplers as bound for the eye, its glare resampled through the focus box (as glare_share.h does
// in the focus passes) and its exposure. So the focus view matches its eye by construction, and the game's code runs as
// it is: no extra pass. The draws are recorded on a deferred context and executed with the game's state restored. Then,
// after the game's final blit, the same blit quad over the focus region beside the eye in the eye's final (blit_final,
// docs/recon-lmu.md "R3b/R3c design"): the focus view's final there, with the game's fades.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "../../quad/d3d11_view_resample.h"

namespace lmu {

// The focus view's part of the resolved scene, in its texels.
struct FocusPostRegion {
    unsigned x = 0, y = 0, width = 0, height = 0;
};

class FocusPost {
public:
    bool initialize(ID3D11Device* device);
    bool ready() const { return ready_; }
    // Right after the eye pass's YEBIS copy of the resolved scene, its pixel shader, constant buffer 0 and sampler 0 still
    // bound on `context` (the immediate context): `scene` (a view of the resolved scene, `sceneWidth` x `sceneHeight`)'s
    // `focus` part copied the same way into eye `eye`'s scene image.
    bool after_copy(ID3D11DeviceContext* context, int eye, ID3D11ShaderResourceView* scene, unsigned sceneWidth,
                    unsigned sceneHeight, const FocusPostRegion& focus);
    // Instead of after_copy, when YEBIS's chain starts from an image a stage between the resolve and YEBIS made (its
    // composite reads that image directly, so no copy of the game's to replay): `scene`'s `focus` part copied texel
    // for texel, with our own pixel shader, into eye `eye`'s scene image.
    bool scene_from(ID3D11DeviceContext* context, int eye, ID3D11ShaderResourceView* scene, unsigned sceneWidth,
                    unsigned sceneHeight, const FocusPostRegion& focus);
    // Right after the eye pass's YEBIS composite, its pixel shader, constant buffer 0, samplers 0-2 and resources still
    // bound (t1 the eye's glare, t2 its 1x1 exposure): the glare's `glareRegion` (the eye's part) resampled through the
    // focus box (`mapping`) into eye `eye`'s glare image, then the composite of the scene image (after_copy, this frame)
    // and that glare into eye `eye`'s focus image (RGBA16F, as the eye's composite). False: no scene image this frame,
    // or a step failed.
    bool after_composite(ID3D11DeviceContext* context, int eye, const simqvfr::ViewRegion& glareRegion,
                         const simqvfr::ViewMapping& mapping);
    // Right after the game's final blit (its Draw(vertexCount, startVertex) with its vertex shader, vertices and target
    // still bound: the eye's composite into the eye's final, the quad over `eyeRegion` of the target): the same quad
    // once more over `focus` (the focus view's region of the same target, beside the eye), with our pixel shader: the
    // focus image (after_composite, this frame) times the game's vertex colour. Directly on `context`, the state it
    // changes put back. False: no composite this frame.
    bool blit_final(ID3D11DeviceContext* context, int eye, UINT vertexCount, UINT startVertex,
                    const FocusPostRegion& eyeRegion, const FocusPostRegion& focus);
    // Eye `eye`'s focus image (after_composite) and the count of composites into it so far (0: none yet).
    ID3D11Texture2D* image(int eye) const { return eye == 0 || eye == 1 ? views_[eye].image.Get() : nullptr; }
    unsigned long long composites(int eye) const { return eye == 0 || eye == 1 ? views_[eye].composites : 0; }
    unsigned long long blits(int eye) const { return eye == 0 || eye == 1 ? views_[eye].blits : 0; }
    void shutdown();

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    struct Image {
        Com<ID3D11Texture2D> texture;
        Com<ID3D11ShaderResourceView> srv;
        Com<ID3D11RenderTargetView> rtv;
        unsigned width = 0, height = 0;
    };
    struct View {
        Image scene, glare, focus;  // the copy's output, the glare, the composite's (all RGBA16F)
        Com<ID3D11Texture2D> image;  // = focus.texture
        bool sceneReady = false;    // after_copy ran since the last composite
        bool composited = false;    // after_composite ran since the last blit
        unsigned sceneWidth = 0, sceneHeight = 0;
        unsigned long long composites = 0, blits = 0;
    };
    bool ensure(Image& image, unsigned width, unsigned height, DXGI_FORMAT format);
    // The game's constant buffer 0 as bound now, copied (the game rewrites its buffer for every draw).
    ID3D11Buffer* snapshot(ID3D11DeviceContext* context, Com<ID3D11Buffer>& keep);
    bool run(ID3D11DeviceContext* context);  // the deferred context's draws, executed with the game's state restored
    // after_copy and scene_from: `ps` with `constants` at b0 (and `sampler` at s0, if any) over `focus` of `scene`.
    bool copy_scene(ID3D11DeviceContext* context, int eye, ID3D11ShaderResourceView* scene, unsigned sceneWidth,
                    unsigned sceneHeight, const FocusPostRegion& focus, ID3D11PixelShader* ps, ID3D11Buffer* constants,
                    ID3D11SamplerState* sampler);

    bool ready_ = false;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> deferred_;
    Com<ID3D11VertexShader> copyVs_, compositeVs_;
    Com<ID3D11PixelShader> blitPs_, scenePs_;  // scenePs_: scene_from's texel copy
    Com<ID3D11Buffer> sceneOrigin_;            // scene_from: the focus part's top-left in the scene
    Com<ID3D11Buffer> rects_;   // the copy's source rect in UV (offset, size)
    Com<ID3D11Buffer> origin_;  // blit_final: the focus region's top-left
    Com<ID3D11Buffer> copyConstants_, compositeConstants_;
    Com<ID3D11RasterizerState> raster_;
    Com<ID3D11BlendState> blend_;
    Com<ID3D11DepthStencilState> depth_;
    simqvfr::D3D11ViewResample resample_;
    View views_[2];
};

}  // namespace lmu
