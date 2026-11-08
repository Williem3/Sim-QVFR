// The helmet overlay: its pieces, the visor, its layer's resources, and the views4 helmet.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "log.h"
#include "quad/compositor.h"
#include "quad/focus_turn.h"
#include "quad/fov_crop.h"
#include "quad/helmet.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

// The helmet's pieces (under quad.mutex): each image, relative to the layer's folder, loaded into the
// compositor when its path changes or, looked at once a second, its file appears, changes or goes (a
// load stalls the GPU once); a missing file is no piece. So images can be dropped in or edited while
// the game runs.
void helmet_pieces(const HelmetSettings& h) {
    const int64_t now = qpc_now();
    const std::wstring* paths[3] = {&h.visorImage, &h.bottomImage, &h.topImage};  // HelmetPiece order
    const bool look = now >= quad.helmetPiecesCheck;
    if (look)
        quad.helmetPiecesCheck = now + qpc_ms(1000);
    for (int p = 0; p < 3; ++p) {
        const bool samePath = *paths[p] == quad.helmetPiecesTried[p];
        if (samePath && !look)
            continue;
        const std::filesystem::path image(*paths[p]);
        const std::filesystem::path full = image.is_absolute() ? image : std::filesystem::path(layer_directory()) / image;
        std::error_code error;
        std::filesystem::file_time_type written = std::filesystem::file_time_type::min();
        if (!paths[p]->empty() && std::filesystem::is_regular_file(full, error)) {
            written = std::filesystem::last_write_time(full, error);
            if (error)
                written = std::filesystem::file_time_type::min();
        }
        if (samePath && written == quad.helmetPiecesWritten[p])
            continue;
        const bool first = !samePath;
        quad.helmetPiecesTried[p] = *paths[p];
        quad.helmetPiecesWritten[p] = written;
        const bool exists = written != std::filesystem::file_time_type::min();
        helmet_set().load_helmet_piece(static_cast<HelmetPiece>(p), exists ? full.wstring() : std::wstring());
        if (!paths[p]->empty() && !exists && first)
            logf("helmet: no %ls (that piece is left out until it appears)", full.c_str());
    }
    quad.helmetPiecesLoaded = helmet_set().has_helmet_pieces();
}

// The visor's slide this frame (under quad.mutex): it moves towards the asked position, all the way
// from closed to open in kVisorSlideS.
constexpr double kVisorSlideS = 0.3;

double visor_step(const HelmetSettings& h) {
    const double target = visor_target(h);
    const int64_t now = qpc_now();
    if (quad.visorPosition < 0) {
        quad.visorPosition = target;
    } else {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const double step = static_cast<double>(now - quad.visorAt) / frequency.QuadPart / kVisorSlideS;
        quad.visorPosition = quad.visorPosition < target ? std::min(target, quad.visorPosition + step)
                                                         : std::max(target, quad.visorPosition - step);
    }
    quad.visorAt = now;
    return quad.visorPosition;
}

// The helmet layer's swapchain and VIEW space (under quad.mutex), once the pieces are loaded; square,
// helmet_layer_side ([helmet] detail: 2048 or 4096) a side.
bool helmet_layer_resources(XrSession session) {
    if (quad.helmetChain)
        return true;
    if (quad.helmetLayerFailed)
        return false;
    const uint32_t side = helmet_layer_side(helmetSettings);
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    chainInfo.format = kHelmetLayerFormat;
    chainInfo.sampleCount = 1;
    chainInfo.width = chainInfo.height = side;
    chainInfo.faceCount = chainInfo.arraySize = chainInfo.mipCount = 1;
    uint32_t count = 0;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &quad.helmetSpace);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &quad.helmetChain);
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(quad.helmetChain, 0, &count, nullptr);
    const bool d3d11 = experiment.views4ComposeD3D11;
    std::vector<XrSwapchainImageD3D12KHR> images(d3d11 ? 0 : count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    std::vector<XrSwapchainImageD3D11KHR> images11(d3d11 ? count : 0, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r) && count)
        r = next.EnumerateSwapchainImages(quad.helmetChain, count, &count,
                                          d3d11 ? reinterpret_cast<XrSwapchainImageBaseHeader*>(images11.data())
                                                : reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r) || !count) {
        logf("helmet: its own layer failed (%d); the pieces are drawn into the scene image instead (frame "
             "generation may then smear)", r);
        if (quad.helmetChain)
            next.DestroySwapchain(quad.helmetChain);
        if (quad.helmetSpace)
            next.DestroySpace(quad.helmetSpace);
        quad.helmetChain = XR_NULL_HANDLE;
        quad.helmetSpace = XR_NULL_HANDLE;
        quad.helmetLayerFailed = true;
        return false;
    }
    quad.helmetImages.clear();
    quad.helmetImages11.clear();
    for (const auto& image : images)
        quad.helmetImages.push_back(image.texture);
    for (const auto& image : images11)
        quad.helmetImages11.push_back(image.texture);
    for (XrCompositionLayerQuad& l : quad.helmetQuads) {
        l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
        l.space = quad.helmetSpace;
        l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;  // per quad in helmet_layer_draw
        l.subImage.swapchain = quad.helmetChain;
    }
    quad.helmetSide = side;
    logf("helmet: own head-locked layer, %ux%u (%u images; %s)", side, side, count,
         side == kHelmetDetailSide ? "detail" : "performance");
    return true;
}

}  // namespace

// The helmet's layer goes (under quad.mutex): its swapchain and space, made again when needed.
void helmet_layer_release() {
    if (quad.helmetChain)
        next.DestroySwapchain(quad.helmetChain);
    if (quad.helmetSpace)
        next.DestroySpace(quad.helmetSpace);
    quad.helmetChain = XR_NULL_HANDLE;
    quad.helmetSpace = XR_NULL_HANDLE;
    quad.helmetImages.clear();
    quad.helmetImages11.clear();
    quad.helmetSide = 0;
    quad.helmetQuadCount = 0;
    quad.helmetDrawn = quad.helmetResize = false;
}

namespace {

// The helmet layer for this frame (under quad.mutex, on ACE's queue after the composition): the quad
// placed at the helmet's distance and height, sized to cover both eyes' uncropped views (`full`), and the
// pieces drawn into it. False if it was not drawn (nothing is submitted then). `views11`: a D3D11 game's image
// of its views (the D3D11 compositor draws it, the frame's light from it); null: ACE's D3D12 compositor.
bool helmet_layer_draw(const HelmetSettings& h, const EyeComposition (&eyes)[2], const float (&full)[2][4],
                       double visorOffset, ID3D11Texture2D* views11) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const size_t images = views11 ? quad.helmetImages11.size() : quad.helmetImages.size();
    if (XR_FAILED(next.AcquireSwapchainImage(quad.helmetChain, &acquire, &index)) || index >= images)
        return false;
    const double tilt = h.height * kHelmetDegrees, distance = helmet_distance(h);
    const XrQuaternionf up{static_cast<float>(std::sin(tilt / 2)), 0, 0, static_cast<float>(std::cos(tilt / 2))};
    float rotation[2][9], offset[2][3];
    for (int e = 0; e < 2; ++e) {
        to_matrix(qmul(qconj(up), quad.v4EyeTurn[e]), rotation[e]);
        const XrVector3f o = rotate(qconj(up), quad.v4EyeOffset[e]);  // in the images' frame
        offset[e][0] = o.x;
        offset[e][1] = o.y;
        offset[e][2] = o.z;
    }
    double coverX, coverY, imageX, imageY;
    helmet_cover(rotation, offset, full, distance, coverX, coverY);
    helmet_tan_half(h, helmet_set().helmet_aspect(), imageX, imageY);
    // Only the bands the pieces draw on, with the visor where it is now.
    const std::vector<HelmetBand> bands =
        helmet_bands(helmet_set().helmet_rows(visorOffset, h.visorBlend), imageY, coverY);
    HelmetLayerDraw draw{};
    draw.coverTan[0] = static_cast<float>(coverX);
    draw.coverTan[1] = static_cast<float>(coverY);
    draw.imageTan[0] = static_cast<float>(imageX);
    draw.imageTan[1] = static_cast<float>(imageY);
    draw.visorOffset = static_cast<float>(visorOffset);
    draw.light = static_cast<float>(h.light) / 100.0f;
    draw.visorBlend = h.visorBlend;
    draw.checkLight = h.debugLight;
    std::memcpy(draw.outerSource[0], eyes[0].outerSource, sizeof(draw.outerSource[0]));
    std::memcpy(draw.outerSource[1], eyes[1].outerSource, sizeof(draw.outerSource[1]));
    draw.bands = bands;
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(quad.helmetChain, &wait)) &&
                    (views11 ? v4d3d11.compositor.draw_helmet_layer(views11, quad.helmetImages11[index], draw)
                             : quad.compositor.draw_helmet_layer(quad.helmetImages[index], draw));
    next.ReleaseSwapchainImage(quad.helmetChain, &release);
    // The quads, in VIEW space: the whole quad centred `height` above straight ahead, facing the eyes; each band its
    // slice of it (along the quad's own up) and of the image. Always kHelmetFarM away (helmet_far_centre: SteamVR drew
    // a nearer helmet over the settings menu and the overlay, 2026-11-08); a near depth by one copy per eye.
    const double scale = helmet_far_scale(distance);
    const bool perEye = scale > 1.0001;
    const float width = static_cast<float>(2 * distance * coverX), height = static_cast<float>(2 * distance * coverY);
    const int32_t side = static_cast<int32_t>(quad.helmetSide);
    quad.helmetQuadCount = 0;
    int bandCount = 0;
    for (const HelmetBand& b : bands) {
        if (bandCount++ == 2)
            break;
        const int32_t top = static_cast<int32_t>(std::floor(b.top * side)), bottom = static_cast<int32_t>(std::ceil(b.bottom * side));
        const float rise = (0.5f - 0.5f * (top + bottom) / side) * height;  // the band's centre above the quad's
        for (int e = 0; e < (perEye ? 2 : 1); ++e) {
            XrCompositionLayerQuad& l = quad.helmetQuads[quad.helmetQuadCount++];
            const float none[3] = {0, 0, 0};
            float at[3];
            helmet_far_centre(distance, rise, perEye ? offset[e] : none, at);
            l.eyeVisibility = !perEye ? XR_EYE_VISIBILITY_BOTH : e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
            l.pose.orientation = up;
            l.pose.position = rotate(up, XrVector3f{at[0], at[1], at[2]});
            l.size = {static_cast<float>(width * scale), static_cast<float>(height * scale * (bottom - top) / side)};
            l.subImage.imageRect = {{0, top}, {side, bottom - top}};
        }
    }
    char text[96] = "whole";
    if (bands.empty()) {
        std::snprintf(text, sizeof(text), "none (no piece in view)");
    } else if (bands.size() != 1 || bands[0].top > 0 || bands[0].bottom < 1) {
        int n = 0;
        double drawn = 0;
        for (const HelmetBand& b : bands) {
            n += std::snprintf(text + n, sizeof(text) - n, "%s%.2f-%.2f", n ? ", " : "", b.top, b.bottom);
            drawn += b.bottom - b.top;
        }
        std::snprintf(text + n, sizeof(text) - n, " (%.0f%% of it)", drawn * 100);
    }
    if (quad.helmetBandsLogged != text) {
        quad.helmetBandsLogged = text;
        logf("helmet: layer drawn and submitted over %s of the quad (top to bottom)", text);
    }
    return ok;
}

// Once per change of the pieces or their size and height (under quad.mutex): the crop that meets the
// pieces' opening straight ahead, for the log (`full`: eye 0's uncropped view).
void helmet_crop_log(const HelmetSettings& h, const float (&full)[4]) {
    const double top = helmet_set().helmet_edge(HelmetPiece::Top), bottom = helmet_set().helmet_edge(HelmetPiece::Bottom);
    const std::wstring key = quad.helmetPiecesTried[1] + L"|" + quad.helmetPiecesTried[2] + L"|" +
                             std::to_wstring(quad.helmetPiecesWritten[1].time_since_epoch().count()) + L"|" +
                             std::to_wstring(quad.helmetPiecesWritten[2].time_since_epoch().count()) + L"|" +
                             std::to_wstring(h.size) + L"|" + std::to_wstring(h.height);
    if (key == quad.helmetHinted || (top < 0 && bottom < 0))
        return;
    quad.helmetHinted = key;
    int cropTop = 0, cropBottom = 0;
    helmet_crop_hint(h, helmet_set().helmet_aspect(), top, bottom < 0 ? -1 : 1 - bottom, full[2], full[3], cropTop,
                     cropBottom);
    logf("helmet: pieces at size %d deg, height %d deg: their opening straight ahead meets [crop] top=%d bottom=%d",
         h.size, h.height, cropTop, cropBottom);
}

}  // namespace

// With the helmet on, every 10 s (under quad.mutex): the helmet's light estimate in the log, per eye and
// side, linear RGB, so a log shows whether it follows the scene (a tunnel, a bright sky). The copy is
// asked for here and logged at a later composition, once the GPU has made it.
void log_helmet_light() {
    float rgb[2][4][3];
    if (helmet_set().read_light(rgb)) {
        char text[512];
        int n = 0;
        static const char* const kSides[4] = {"left", "right", "top", "bottom"};
        for (int e = 0; e < 2; ++e) {
            n += std::snprintf(text + n, sizeof(text) - n, "%seye %d", e ? "; " : "", e);
            for (int s = 0; s < 4; ++s)
                n += std::snprintf(text + n, sizeof(text) - n, " %s %.3f/%.3f/%.3f", kSides[s], rgb[e][s][0],
                                   rgb[e][s][1], rgb[e][s][2]);
        }
        logf("helmet light (linear RGB per side of the rendered view): %s", text);
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= quad.helmetLightLogAt) {
        helmet_set().request_light();
        quad.helmetLightLogAt = now + 10000;
    }
}

V4Helmet views4_helmet_start(const HelmetSettings& helmet) {
    V4Helmet h;
    if (helmet.on)
        helmet_pieces(helmet);
    h.visorOffset = helmet.on ? visor_offset(helmet, visor_step(helmet)) : 0;
    h.pieces = helmet.on && helmet_set().has_helmet_pieces();
    h.piecesInScene = h.pieces && quad.helmetLayerFailed;
    return h;
}

// Eye e's helmet and output (as views4_helmet_start), once views4_eye has filled `c` from the frame: the FOV the
// output is submitted with, and the eye's size in the output from the top left of its half (native, or with the
// FOV crop its kept share; with the pieces in the scene, the whole uncropped eye at native size).
void views4_helmet_eye(const HelmetSettings& helmet, V4Helmet& h, int e, EyeComposition& c, XrFovf& submitFov,
                       XrExtent2Di& extent) {
    // The output covers the outer view's tangents, so that is the FOV it is submitted with.
    submitFov = tangents_fov(c.outerTan);
    float full[4];
    uncrop_tangents(c.outerTan, quad.v4Crop, e, full);
    std::memcpy(h.fullView[e], full, sizeof(full));
    if (h.pieces) {
        // The crop hint once the game renders the located view (not its own projection on the loading screens).
        float located[4];
        fov_tangents(quad.v4Fov[e], located);
        bool rendersLocated = true;
        for (int i = 0; i < 4; ++i)
            rendersLocated &= std::fabs(c.outerTan[i] - located[i]) < 0.01f * std::max(1.0f, std::fabs(located[i]));
        if (e == 0 && rendersLocated)
            helmet_crop_log(helmet, full);
    }
    if (helmet.on && !h.pieces) {
        // The built-in look, inside the rendered view: its lip reaches in from the opening's edge.
        const HelmetShape shape = helmet_shape(helmet, full, c.outerTan);
        std::memcpy(c.helmetOpening, shape.opening, sizeof(shape.opening));
        c.helmetRim = shape.rim;
        c.helmetRadius = shape.radius;
        c.helmetLight = static_cast<float>(helmet.light) / 100.0f;
        c.helmet = helmet.debugLight ? 3 : 1;
    }
    if (h.piecesInScene) {
        // Fallback (no layer of their own): the output covers the whole uncropped eye at native size;
        // the pieces fill what the game did not render.
        std::memcpy(c.viewTan, full, sizeof(full));
        submitFov = tangents_fov(full);
        extent = {static_cast<int32_t>(quad.nativeW), static_cast<int32_t>(quad.nativeH)};
        const HelmetShape shape = helmet_shape(helmet, full, c.outerTan);
        std::memcpy(c.helmetOpening, shape.opening, sizeof(shape.opening));
        c.helmetRim = shape.rim;
        c.helmetRadius = shape.radius;
        c.helmetLight = static_cast<float>(helmet.light) / 100.0f;
        c.helmet = helmet.debugLight ? 3 : 2;
        // The pieces, fixed to the head: the eye's turn against the head, then the images' height.
        const double tilt = helmet.height * kHelmetDegrees;
        const XrQuaternionf up{static_cast<float>(std::sin(tilt / 2)), 0, 0, static_cast<float>(std::cos(tilt / 2))};
        to_matrix(qmul(qconj(up), quad.v4EyeTurn[e]), c.helmetRotation);
        double tx, ty;
        helmet_tan_half(helmet, helmet_set().helmet_aspect(), tx, ty);
        c.helmetTanHalf[0] = static_cast<float>(tx);
        c.helmetTanHalf[1] = static_cast<float>(ty);
        const XrVector3f offset = rotate(qconj(up), quad.v4EyeOffset[e]);  // in the images' frame
        const float eyeOffset[3] = {offset.x, offset.y, offset.z};
        helmet_parallax(helmet, eyeOffset, c.helmetShift);
        c.visorOffset = static_cast<float>(h.visorOffset);
        c.visorBlend = helmet.visorBlend;
        return;
    }
    // The FOV crop: the output keeps native density over the cropped FOV (views4_output_extent).
    extent = views4_output_extent();
    c.outputSize[0] = static_cast<float>(extent.width) / static_cast<float>(quad.nativeW);
    c.outputSize[1] = static_cast<float>(extent.height) / static_cast<float>(quad.nativeH);
}

// After a composition (as views4_helmet_start): the helmet's own layer drawn for it, unless its size changed
// ([helmet] detail: the next submission makes it again). `views11`: as helmet_layer_draw's.
void views4_helmet_layer(const HelmetSettings& helmet, const V4Helmet& h, const EyeComposition (&eyes)[2],
                         ID3D11Texture2D* views11) {
    if (quad.helmetChain && helmet_layer_side(helmet) != quad.helmetSide)
        quad.helmetResize = true;
    quad.helmetDrawn = h.pieces && !h.piecesInScene && quad.helmetChain && !quad.helmetResize &&
                       helmet_layer_draw(helmet, eyes, h.fullView, h.visorOffset, views11);
}

// At the submission of composed projection layer `layer` (under quad.mutex): the helmet layer drawn for it goes
// over it (`quads`, `count`, `after`); its swapchain goes when its size changed, and is made once there are pieces
// (drawn from the next composition on).
void views4_helmet_submit(XrSession session, int layer, XrCompositionLayerQuad (&quads)[kHelmetQuadsMax], int& count,
                          int& after) {
    if (quad.helmetDrawn) {
        count = quad.helmetQuadCount;
        std::copy(quad.helmetQuads, quad.helmetQuads + count, quads);
        after = layer;
    }
    quad.helmetDrawn = false;
    if (quad.helmetResize)
        helmet_layer_release();  // nothing of it submitted this frame
    if (helmetSettings.on && quad.helmetPiecesLoaded)
        helmet_layer_resources(session);
}

}  // namespace simqvfr::layer
