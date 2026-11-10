// D3D12 quad-view compositor: keeps a copy of frame A's image (full FOV, "outer") and draws the
// final stereo image as the outer view upscaled to the full FOV with frame B's image (focus FOV)
// blended into the focus rectangle.
//
// Images follow XR_KHR_D3D12_enable: OpenXR swapchain images are in RENDER_TARGET state between
// acquire and release, and are left in that state.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <wrl/client.h>

#include "../xr.h"
#include "helmet.h"
#include "helmet_pieces.h"

namespace simqvfr {

// Frame B may look in a different direction than frame A (its camera turned towards the gaze, with
// a centred projection): every output pixel's ray in A's eye frame is rotated into B's frame and
// projected with B's tangents, so the focus area is wherever B's view lands, exactly, at any depth.
struct EyeComposition {
    float outerTan[4];    // frame A's view tangents: left, right, up, down
    float focusTan[4];    // frame B's view tangents: left, right, up, down
    float rotation[9];    // row-major; takes a direction in A's eye frame into B's eye frame
    float outerSource[4]; // outer texture region for this eye: u0, v0, du, dv
    float focusSource[4]; // focus texture region for this eye: u0, v0, du, dv
    float feather;        // blend band, as a fraction of the focus box (0..0.5)
    float border;         // debug: width of an outline at the box edge, as a fraction of the box (0 = off)
    float borderColor[4]; // its colour (linear RGBA)
    float cornerRadius;   // rounded box corners: radius as a share of half the box's shorter side (0 = square)
    // The focus area's corners pulled in (focus_shape.h): top left, top right, bottom right, bottom left, each
    // across then up or down, as shares of the box (0..0.45). Any nonzero: that quadrilateral, without
    // cornerRadius; all 0: the box.
    float cornerInset[8];
    float outputSize[2];  // the share of the eye's half of the output drawn, from its top left (FOV crop); 0 = all
    // The helmet overlay (helmet.h). viewTan: the tangents the eye's output covers (left, right, up, down;
    // all 0 = outerTan); with the helmet on, the uncropped eye, and what ACE did not render is the shell.
    float viewTan[4];
    // The focus layer under native quad views (draw_focus_layer, FocusLayerPS; viewTan.x 1): the tangents its target
    // covers in the eye's frame (left, right, up, down), the runtime's focus view, the box inside; all 0: the box.
    float regionTan[4];
    float helmetOpening[4];  // the visor opening's tangents (left, right, up, down)
    float helmetRim, helmetRadius;  // the lip's width into the opening and the opening's corner radius, in tangents
    float helmetLight;     // how much the frame lights it (0..1)
    int helmet;            // 0 off, 1 the built-in look, 2 the loaded pieces (load_helmet_piece), 3 the frame's light
                           // itself (in the pieces' shapes when they are loaded)
    // Look 2: the pieces are fixed to the head. helmetRotation takes a direction in this eye's frame into
    // the images' (row-major: the eye's turn against the head, then the images' height); they span
    // tangents +-helmetTanHalf; the visor slides up by visorOffset image heights.
    float helmetRotation[9];
    float helmetTanHalf[2];
    float helmetShift[2];  // depth: this eye's shift of the images' tangents (helmet.h's helmet_parallax)
    float visorOffset;
    // The visor: 0 auto (a tint when its image has no transparency, else blended as drawn), 1 always a tint
    // (the scene filtered by its colour, like glass), 2 always blended as drawn.
    int visorBlend;
    // Sharpening (compose_shader.h's cas_sample, AMD's CAS): 0 off (the views resampled bilinear, as before), 1 the most.
    float sharpness = 0;
};

// The helmet as its own layer (helmet.h): the pieces, lit by the frame, drawn into a head-locked quad that
// the runtime composites over the scene, after any frame generation. The quad spans tangents
// +-coverTan of the images' plane; the images +-imageTan of it.
struct HelmetLayerDraw {
    float coverTan[2];
    float imageTan[2];
    float visorOffset;     // the visor's slide, image heights
    float light;           // how much the frame lights it (0..1)
    int visorBlend;        // as EyeComposition's
    bool checkLight;       // the light diagnostic: the pieces' shapes in the frame's light
    float outerSource[2][4];  // each eye's outer view in the eye image (the frame's light)
    std::vector<HelmetBand> bands;  // the parts of the quad to draw (helmet_bands; empty: all of it)
};
// The helmet layer's format (premultiplied alpha, sRGB-encoded).
inline constexpr DXGI_FORMAT kHelmetLayerFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

class QuadCompositor : public HelmetPieceSet {
public:
    // viewFormat is the swapchain's colour format (e.g. R8G8B8A8_UNORM_SRGB); views use it so
    // filtering happens in linear space and the output is re-encoded exactly like ACE's own.
    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT viewFormat);
    // Tests: initialize fails (as on a shader that does not compile), to check the layer's fallback.
    static inline bool failInitialize = false;
    // Copies frame A's image into the outer texture (created on first use with the image's description).
    bool capture_outer(ID3D12Resource* appImage);
    bool has_outer() const { return outerValid_; }
    // Draws both eyes into `output` (a double-wide swapchain image), side by side.
    bool compose(ID3D12Resource* focusImage, ID3D12Resource* output, const EyeComposition (&eyes)[2]);
    // Draws the helmet layer into `target` (a kHelmetLayerFormat swapchain image in RENDER_TARGET state):
    // the frame's light (from the outer copy capture_outer made), then the pieces, premultiplied, with alpha
    // = how much of the scene they hide. False without pieces.
    bool draw_helmet_layer(ID3D12Resource* target, const HelmetLayerDraw& draw);
    // The helmet's pieces: HelmetPieceSet (helmet_pieces.h); a load stalls the GPU once.
    // The helmet's light for the log (HelmetPieceSet): copied at the next composition with the helmet on.
    void request_light() override { lightWanted_ = true; }
    bool read_light(float (&rgb)[2][4][3]) override;
    // Debug: saves the left eye of the outer copy, the focus image and the output (both in
    // RENDER_TARGET state) as <prefix>-outer.png, -focus.png, -output.png. Stalls the GPU.
    bool save_captures(ID3D12Resource* focusImage, ID3D12Resource* output, const std::wstring& prefix);
    // Debug, views4: saves the whole four-view image as <prefix>-views.png and the output's left eye
    // as <prefix>-output.png (both in RENDER_TARGET state). Stalls the GPU.
    bool save_views4_captures(ID3D12Resource* views, ID3D12Resource* output, const std::wstring& prefix);
    void shutdown();

protected:
    bool pieces_ready() const override { return ready_; }
    bool upload_piece(int piece, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) override;
    void drop_piece(int piece) override { pieces_[piece].Reset(); }

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    static constexpr UINT kSlots = 8;

    struct Slot {
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> list;
        UINT64 fenceValue = 0;
    };
    Slot* next_slot();
    bool submit(Slot& slot);
    // A slot's shader views (outer copy, `focus`, the light, the pieces); returns the loaded pieces (1 << HelmetPiece).
    UINT bind_sources(UINT slotIndex, ID3D12Resource* focus);
    // Records the helmet's light pass (and read_light's copy); true when it copied for read_light.
    bool record_light(ID3D12GraphicsCommandList* list, const float (&source0)[4], const float (&source1)[4]);
    // Saves the left 1/widthDivisor of the image (2: the left eye of a double-wide image; 1: all of it).
    bool save_left_eye(ID3D12Resource* image, D3D12_RESOURCE_STATES state, const std::wstring& path,
                       UINT widthDivisor = 2);
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(UINT index) const;

    Com<ID3D12Device> device_;
    Com<ID3D12CommandQueue> queue_;
    Com<ID3D12RootSignature> rootSignature_;
    Com<ID3D12PipelineState> pipeline_;
    Com<ID3D12DescriptorHeap> srvHeap_, rtvHeap_;
    Com<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceCounter_ = 0;
    UINT srvIncrement_ = 0, rtvIncrement_ = 0;
    Slot slots_[kSlots];
    UINT slotIndex_ = 0;
    DXGI_FORMAT viewFormat_ = DXGI_FORMAT_UNKNOWN;
    Com<ID3D12Resource> outer_;
    bool outerValid_ = false;
    // The helmet overlay: the frame's light along each eye's sides (4 x 2: left, right, top, bottom per
    // eye, blended over frames by lightPipeline_) and the optional image.
    Com<ID3D12PipelineState> lightPipeline_, helmetLayerPipeline_;
    Com<ID3D12Resource> light_;
    Com<ID3D12Resource> pieces_[3];  // the pieces' textures, HelmetPiece order
    bool lightFresh_ = true;
    Com<ID3D12Resource> lightReadback_;  // read_light's copy of light_
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT lightFootprint_ = {};
    bool lightWanted_ = false;
    UINT64 lightCopyFence_ = 0;  // the copy's fence value (0: none pending)
    bool ready_ = false;
};

}  // namespace simqvfr
