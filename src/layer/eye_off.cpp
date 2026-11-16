// One eye off ([quad] eye_off; quad/eye_off.h): the wrapper on the runtime's xrEndFrame, below everything else of the
// layer's (native quad views included), so it sees the frame exactly as the runtime will, whatever composed it. With
// an eye off its projection views show a small black image of the layer's own (made once a session, transparent black:
// a layer blended by alpha shows nothing there, an opaque one black) and the quad layers show to the other eye only.
#include <memory>

#include "log.h"
#include "quad/eye_off.h"
#include "quad/menu_panel.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

// 64 wide: the D3D12 upload's row pitch (PanelUpload) is a multiple of 256 bytes.
constexpr int kBlackSize = 64;

struct EyeOffSubmit {
    PFN_xrEndFrame below = nullptr;
    XrSession session = XR_NULL_HANDLE;
    XrSwapchain black = XR_NULL_HANDLE;
    bool failed = false;  // the black image couldn't be made this session: the eye shows as rendered
    int logged = kEyeOffNone - 1;  // the eye last logged (none yet)
    uint64_t frames = 0, blankedViews = 0, quadsHidden = 0, quadsDropped = 0;
} eo;

// The black image, made at the first frame with an eye off (on the thread that submits frames, as the overlay's card).
bool black_ready(XrSession session) {
    if (eo.black && eo.session == session)
        return true;
    if (eo.failed && eo.session == session)
        return false;
    eo.session = session;
    eo.failed = true;
    const SessionGraphics graphics = session_graphics_now();
    if (!graphics.valid()) {
        log_line("eye off: no D3D11 or D3D12 device: the eye shows as the game renders it");
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // the overlay's: every runtime here takes it
    info.sampleCount = 1;
    info.width = kBlackSize;
    info.height = kBlackSize;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<void*> images;
    XrResult r = next.CreateSwapchain(session, &info, &chain);
    if (XR_SUCCEEDED(r))
        r = swapchain_textures(graphics, next.EnumerateSwapchainImages, chain, images);
    std::unique_ptr<PanelUpload> upload;
    if (XR_SUCCEEDED(r) && !images.empty())
        upload = make_panel_upload(graphics, kBlackSize, kBlackSize);
    bool ok = XR_SUCCEEDED(r) && upload;
    if (ok) {  // one image released black: the runtime shows the last released one from then on
        uint32_t index = 0;
        PendingImage pending;
        // Timed out: the image stays acquired, not waited for, so it isn't released; the chain goes below.
        ok = acquire_wait(chain, pending, kImageWaitNs, 1, index) == XR_SUCCESS;
        if (ok) {
            const std::vector<uint32_t> pixels(static_cast<size_t>(kBlackSize) * kBlackSize, 0u);
            ok = index < images.size() && upload->upload(images[index], pixels);
            next.ReleaseSwapchainImage(chain, nullptr);
        }
    }
    if (!ok) {
        logf("eye off: the black image couldn't be made (%d, %s): the eye shows as the game renders it", r,
             graphics.name());
        if (chain)
            next.DestroySwapchain(chain);
        return false;
    }
    eo.black = chain;
    eo.failed = false;
    logf("eye off: black image ready (%dx%d, %s)", kBlackSize, kBlackSize, graphics.name());
    return true;
}

XrResult XRAPI_CALL eyeOffEndFrame(XrSession session, const XrFrameEndInfo* info) {
    const int eye = eye_off();
    if (eye != eo.logged) {
        eo.logged = eye;
        logf("eye off: %s (from frame %llu of the submission)", eye_off_name(eye),
             static_cast<unsigned long long>(endFrames));
    }
    if (eye == kEyeOffNone || !info || info->layerCount == 0 || !black_ready(session))
        return eo.below(session, info);
    EyeOffFrame frame;
    eye_off_frame(*info, eye, eo.black, kBlackSize, kBlackSize, frame);
    ++eo.frames;
    eo.blankedViews += frame.blankedViews;
    eo.quadsHidden += frame.quadsHidden;
    eo.quadsDropped += frame.quadsDropped;
    if (eo.frames % 5400 == 1)  // the first frame, then about every minute
        logf("eye off: %llu frames with %s: %llu projection views black, %llu quad layers shown to one eye, %llu left out",
             static_cast<unsigned long long>(eo.frames), eye_off_name(eye),
             static_cast<unsigned long long>(eo.blankedViews), static_cast<unsigned long long>(eo.quadsHidden),
             static_cast<unsigned long long>(eo.quadsDropped));
    XrFrameEndInfo submitted = *info;
    submitted.layers = frame.layers.data();
    submitted.layerCount = static_cast<uint32_t>(frame.layers.size());
    return eo.below(session, &submitted);
}

}  // namespace

void eye_off_install() {
    if (!next.EndFrame || next.EndFrame == eyeOffEndFrame)  // (each instance loads `next` anew)
        return;
    eo.below = next.EndFrame;
    next.EndFrame = eyeOffEndFrame;
}

void eye_off_session_ended() {
    if (eo.black && next.DestroySwapchain)
        next.DestroySwapchain(eo.black);
    if (eo.frames)
        logf("eye off: the session's %llu frames with an eye off", static_cast<unsigned long long>(eo.frames));
    const PFN_xrEndFrame below = eo.below;
    eo = {};
    eo.below = below;
}

}  // namespace simqvfr::layer
