// Offline check of the quad pipeline's frame flow on a WARP device. A fake runtime enforces
// OpenXR's frame and swapchain rules (wait/begin/end order, acquire/release pairing, only released
// images submitted); a fake ACE renders red when it sees the full FOV and green when it sees the
// narrowed focus FOV. After arming, every submitted frame must be this layer's native-size output
// with ACE's full FOV, showing red around a green focus box placed where the focus FOV lies.
#include "../src/xr_layer.cpp"
#include "../src/games/ace/game.h"
#include "../src/games/ace/views4.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <dxgi1_6.h>
#include <wincodec.h>

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kNativeW = 160, kNativeH = 144;  // runtime's recommended eye size (eye_scale floor is 64)
constexpr int64_t kFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
constexpr XrFovf kFullFov{-0.80f, 0.70f, 0.75f, -0.85f};  // left, right, up, down

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT64 fenceValue = 0;

    void flush() {
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(fenceValue, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }
} gpu;

struct FakeChain {
    uint32_t width = 0, height = 0;
    std::vector<ComPtr<ID3D12Resource>> images;
    uint32_t nextIndex = 0, acquired = 0, lastReleased = UINT32_MAX;
    std::vector<uint32_t> outstanding;  // acquired, not yet released, in order
};

struct FakeRuntime {
    int waits = 0, begins = 0, ends = 0, violations = 0;
    bool waited = false, begun = false;
    XrTime time = 1000;
    std::vector<FakeChain> chains;  // handle = index + 1
    // last submitted frame
    XrSwapchain submittedChain = XR_NULL_HANDLE;
    uint32_t submittedImage = 0;
    XrCompositionLayerProjectionView submittedViews[2] = {};
    int submittedLayers = 0;
    int submittedProjectionAt = -1, submittedQuadAt = -1;  // layer indices (-1: none)
    XrCompositionLayerQuad submittedQuad{};                // the last quad layer
    int spaces = 0;
} rt;

void violation(const char* what) {
    if (rt.violations++ < 10)
        std::printf("runtime rule broken: %s\n", what);
}

FakeChain* chain_of(XrSwapchain h) {
    const auto i = reinterpret_cast<uintptr_t>(h);
    return i >= 1 && i <= rt.chains.size() ? &rt.chains[i - 1] : nullptr;
}

// The layer's output chain, to read its pixels: without one (the compositor did not start) the test
// stops with a failure rather than reading through a null chain.
FakeChain& output_chain(XrSwapchain h, const char* where) {
    FakeChain* chain = chain_of(h);
    if (!chain) {
        std::printf("FAIL %s: no output chain (the compositor did not start; see logs)\nRESULT: FAIL\n", where);
        std::exit(1);
    }
    return *chain;
}

XrResult XRAPI_CALL fakeViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity, uint32_t* count,
                              XrViewConfigurationView* views) {
    *count = 2;
    for (uint32_t i = 0; i < capacity && i < 2; ++i) {
        views[i].recommendedImageRectWidth = kNativeW;
        views[i].recommendedImageRectHeight = kNativeH;
        views[i].recommendedSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
    FakeChain c;
    c.width = info->width;
    c.height = info->height;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = info->width;
    d.Height = info->height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;  // runtimes hand out typeless images
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (int i = 0; i < 3; ++i) {
        ComPtr<ID3D12Resource> r;
        if (FAILED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r))))
            return XR_ERROR_RUNTIME_FAILURE;
        c.images.push_back(r);
    }
    rt.chains.push_back(std::move(c));
    *out = reinterpret_cast<XrSwapchain>(static_cast<uintptr_t>(rt.chains.size()));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeEnumerateImages(XrSwapchain h, uint32_t capacity, uint32_t* count,
                                        XrSwapchainImageBaseHeader* images) {
    FakeChain* c = chain_of(h);
    if (!c)
        return XR_ERROR_HANDLE_INVALID;
    *count = static_cast<uint32_t>(c->images.size());
    auto* d3d = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (uint32_t i = 0; i < capacity && i < *count; ++i)
        d3d[i].texture = c->images[i].Get();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeAcquire(XrSwapchain h, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    FakeChain* c = chain_of(h);
    if (!c)
        return XR_ERROR_HANDLE_INVALID;
    if (c->outstanding.size() >= c->images.size())
        return violation("acquire with every image acquired"), XR_ERROR_CALL_ORDER_INVALID;
    *index = c->nextIndex;
    c->outstanding.push_back(*index);
    c->nextIndex = (c->nextIndex + 1) % c->images.size();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeWaitImage(XrSwapchain h, const XrSwapchainImageWaitInfo*) {
    return chain_of(h) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL fakeRelease(XrSwapchain h, const XrSwapchainImageReleaseInfo*) {
    FakeChain* c = chain_of(h);
    if (!c)
        return XR_ERROR_HANDLE_INVALID;
    if (c->outstanding.empty())
        return violation("release with nothing acquired"), XR_ERROR_CALL_ORDER_INVALID;
    c->lastReleased = c->outstanding.front();
    c->outstanding.erase(c->outstanding.begin());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeWait(XrSession, const XrFrameWaitInfo*, XrFrameState* s) {
    ++rt.waits;
    rt.waited = true;
    s->predictedDisplayTime = rt.time += 11111;
    s->predictedDisplayPeriod = 11111;
    s->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeBegin(XrSession, const XrFrameBeginInfo*) {
    ++rt.begins;
    if (!rt.waited || rt.begun)
        violation("begin order");
    rt.waited = false;
    rt.begun = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeEnd(XrSession, const XrFrameEndInfo* info) {
    ++rt.ends;
    if (!rt.begun)
        violation("end without begin");
    rt.begun = false;
    rt.submittedLayers = static_cast<int>(info->layerCount);
    rt.submittedProjectionAt = rt.submittedQuadAt = -1;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            rt.submittedQuad = *reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
            rt.submittedQuadAt = static_cast<int>(i);
            FakeChain* qc = chain_of(rt.submittedQuad.subImage.swapchain);
            if (!qc || qc->lastReleased == UINT32_MAX || !qc->outstanding.empty())
                violation("submitted a quad layer whose swapchain has no released image");
            continue;
        }
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        rt.submittedProjectionAt = static_cast<int>(i);
        auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        FakeChain* c = chain_of(proj->views[0].subImage.swapchain);
        if (!c || c->lastReleased == UINT32_MAX)
            violation("submitted a swapchain with no released image");
        else if (!c->outstanding.empty())
            violation("submitted a swapchain with an image still acquired");
        rt.submittedChain = proj->views[0].subImage.swapchain;
        rt.submittedImage = c ? c->lastReleased : 0;
        rt.submittedViews[0] = proj->views[0];
        rt.submittedViews[1] = proj->views[1];
    }
    return info->displayTime == rt.time ? XR_SUCCESS : XR_ERROR_TIME_INVALID;
}
// A helmet top piece for the flow: 8 x 8, its first two rows opaque white.
void write_file_png(const std::wstring& path) {
    BYTE bgra[8 * 8 * 4] = {};
    for (int i = 0; i < 2 * 8 * 4; ++i)
        bgra[i] = 255;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(8, 8)) && SUCCEEDED(frame->SetPixelFormat(&format)))
        frame->WritePixels(8, 8 * 4, sizeof(bgra), bgra), frame->Commit(), encoder->Commit();
}

XrResult XRAPI_CALL fakeCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* space) {
    *space = reinterpret_cast<XrSpace>(static_cast<uintptr_t>(0x5000 + ++rt.spaces));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeDestroySpace(XrSpace) { return XR_SUCCESS; }
XrResult XRAPI_CALL fakeDestroySwapchain(XrSwapchain) { return XR_SUCCESS; }
XrResult XRAPI_CALL fakeLocate(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t capacity, uint32_t* count,
                               XrView* views) {
    *count = 2;
    for (uint32_t i = 0; i < capacity && i < 2; ++i) {
        views[i].pose = {{0, 0, 0, 1}, {i ? 0.032f : -0.032f, 1.2f, 0}};
        views[i].fov = kFullFov;
    }
    return XR_SUCCESS;
}

// Eye gaze: the fake tracker looks at tangent (kGazeX, kGazeY) of the eye views.
constexpr double kGazeX = 0.5, kGazeY = -0.4;
const XrActionSet kAceSet = reinterpret_cast<XrActionSet>(0x800);
const XrAction kAceAction = reinterpret_cast<XrAction>(0x900);
struct FakeInput {
    std::map<std::string, XrPath> paths;
    uint32_t suggestedBindings = 0, attachedSets = 0, syncedSets = 0;
    int syncs = 0;
    uintptr_t nextAction = 0x200;           // each created action gets its own handle
    std::vector<XrAction> suggestedActions;  // the last eye gaze suggestion's actions
} input;

XrResult XRAPI_CALL fakeStringToPath(XrInstance, const char* s, XrPath* path) {
    auto [it, fresh] = input.paths.emplace(s, input.paths.size() + 1);
    *path = it->second;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* set) {
    *set = reinterpret_cast<XrActionSet>(0x100);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeCreateAction(XrActionSet, const XrActionCreateInfo*, XrAction* action) {
    *action = reinterpret_cast<XrAction>(input.nextAction++);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeSuggest(XrInstance, const XrInteractionProfileSuggestedBinding* s) {
    input.suggestedBindings = s->countSuggestedBindings;
    if (s->interactionProfile == input.paths["/interaction_profiles/ext/eye_gaze_interaction"]) {
        input.suggestedActions.clear();
        for (uint32_t i = 0; i < s->countSuggestedBindings; ++i)
            input.suggestedActions.push_back(s->suggestedBindings[i].action);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeAttach(XrSession, const XrSessionActionSetsAttachInfo* info) {
    if (input.attachedSets)
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    input.attachedSets = info->countActionSets;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeSync(XrSession, const XrActionsSyncInfo* info) {
    ++input.syncs;
    input.syncedSets = info->countActiveActionSets;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeCreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* space) {
    *space = reinterpret_cast<XrSpace>(0x300);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
    // Rotation taking a = (0,0,-1) to the gaze direction b: q = normalize(1 + a.b, a x b), where
    // a.b = -bz and a x b = (by, -bx, 0).
    const double len = std::sqrt(kGazeX * kGazeX + kGazeY * kGazeY + 1);
    const double bx = kGazeX / len, by = kGazeY / len, bz = -1 / len;
    const double w = 1 - bz, x = by, y = -bx;
    const double n = std::sqrt(w * w + x * x + y * y);
    location->pose.orientation = {static_cast<float>(x / n), static_cast<float>(y / n), 0.0f, static_cast<float>(w / n)};
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    return XR_SUCCESS;
}

// Fake ACE: one complete frame, red for the full FOV and green for a narrowed one.
XrResult ace_frame(XrSwapchain chain, uint32_t eyeW, uint32_t eyeH) {
    using namespace aceqv;
    XrFrameState state{XR_TYPE_FRAME_STATE};
    layerWaitFrame(nullptr, nullptr, &state);
    layerBeginFrame(nullptr, nullptr);
    const XrActiveActionSet active{kAceSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    layerSyncActions(nullptr, &sync);
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    layerLocateViews(nullptr, &locate, &viewState, 2, &count, views);

    uint32_t index = 0;
    layerAcquireSwapchainImage(chain, nullptr, &index);
    const bool narrowed = views[0].fov.angleLeft > kFullFov.angleLeft + 0.01f;
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(chain_of(chain)->images[index].Get(), &v, rtv);
    gpu.list->ClearRenderTargetView(rtv, narrowed ? green : red, 0, nullptr);
    gpu.flush();
    layerReleaseSwapchainImage(chain, nullptr);

    XrCompositionLayerProjectionView pv[2];
    for (int e = 0; e < 2; ++e) {
        pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        pv[e].pose = views[e].pose;
        pv[e].fov = views[e].fov;
        pv[e].subImage = {chain, {{static_cast<int32_t>(e * eyeW), 0}, {static_cast<int32_t>(eyeW),
                                                                        static_cast<int32_t>(eyeH)}}, 0};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.viewCount = 2;
    layer.views = pv;
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.layerCount = 1;
    end.layers = layers;
    return layerEndFrame(nullptr, &end);
}

// Fake ACE in views4: one frame drawing the four views where the layout puts them (outer views red
// and blue, focus views green and yellow, the unused rest of the target magenta), submitted as ACE
// does: the two halves of its double-wide target.
XrResult ace_views4_frame(XrSwapchain chain, uint32_t eyeW, uint32_t eyeH) {
    using namespace aceqv;
    XrFrameState state{XR_TYPE_FRAME_STATE};
    layerWaitFrame(nullptr, nullptr, &state);
    layerBeginFrame(nullptr, nullptr);
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    layerLocateViews(nullptr, &locate, &viewState, 2, &count, views);

    uint32_t index = 0;
    layerAcquireSwapchainImage(chain, nullptr, &index);
    const float magenta[4] = {1, 0, 1, 1};
    const float colors[4][4] = {{1, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 0, 1}, {1, 1, 0, 1}};
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(chain_of(chain)->images[index].Get(), &v, rtv);
    gpu.list->ClearRenderTargetView(rtv, magenta, 0, nullptr);
    for (int i = 0; i < 4; ++i) {
        const ViewRect r = views4_rect(experiment.views4Layout, i, static_cast<int>(2 * eyeW), static_cast<int>(eyeH));
        const D3D12_RECT rect{r.left, 0, r.left + r.width, r.height};
        gpu.list->ClearRenderTargetView(rtv, colors[i], 1, &rect);
    }
    gpu.flush();
    layerReleaseSwapchainImage(chain, nullptr);

    XrCompositionLayerProjectionView pv[2];
    for (int e = 0; e < 2; ++e) {
        pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        pv[e].pose = views[e].pose;
        pv[e].fov = views[e].fov;
        pv[e].subImage = {chain, {{static_cast<int32_t>(e * eyeW), 0}, {static_cast<int32_t>(eyeW),
                                                                        static_cast<int32_t>(eyeH)}}, 0};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.viewCount = 2;
    layer.views = pv;
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.layerCount = 1;
    end.layers = layers;
    return layerEndFrame(nullptr, &end);
}

// Fake ACE in views4, rendering a FOV of its own: each view is split at a tangent (x = `split`
// for a vertical boundary, or y = `split` for a horizontal one) into two colours, the outer views
// rendered with `rendered` tangents (red | blue), the focus views with that frame's focus tangents
// (green | yellow), the unused rest black. The frame's record goes to the composition first, as
// fill_focus_cameras would push it. If the composition maps both views with the geometry they were
// rendered with, both boundaries land on the same output tangent.
XrResult ace_views4_split_frame(XrSwapchain chain, uint32_t eyeW, uint32_t eyeH, const aceqv::Views4Frame& frame,
                                bool vertical, float split) {
    using namespace aceqv;
    XrFrameState state{XR_TYPE_FRAME_STATE};
    layerWaitFrame(nullptr, nullptr, &state);
    layerBeginFrame(nullptr, nullptr);
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    layerLocateViews(nullptr, &locate, &viewState, 2, &count, views);
    views4_push_frame(frame);

    uint32_t index = 0;
    layerAcquireSwapchainImage(chain, nullptr, &index);
    const float black[4] = {0, 0, 0, 1};
    const float colors[2][2][4] = {{{1, 0, 0, 1}, {0, 0, 1, 1}}, {{0, 1, 0, 1}, {1, 1, 0, 1}}};  // [focus][side]
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(chain_of(chain)->images[index].Get(), &v, rtv);
    gpu.list->ClearRenderTargetView(rtv, black, 0, nullptr);
    for (int i = 0; i < 4; ++i) {
        const ViewRect r = views4_rect(experiment.views4Layout, i, static_cast<int>(2 * eyeW), static_cast<int>(eyeH));
        const float* t = i < 2 ? frame.outerTan[i] : frame.focusTan[i - 2];
        const bool focus = i >= 2;
        if (vertical) {
            const LONG x = r.left + static_cast<LONG>(std::lround((split - t[0]) / (t[1] - t[0]) * r.width));
            const D3D12_RECT a{r.left, 0, x, r.height}, b{x, 0, r.left + r.width, r.height};
            gpu.list->ClearRenderTargetView(rtv, colors[focus][0], 1, &a);
            gpu.list->ClearRenderTargetView(rtv, colors[focus][1], 1, &b);
        } else {
            const LONG y = static_cast<LONG>(std::lround((t[2] - split) / (t[2] - t[3]) * r.height));
            const D3D12_RECT a{r.left, 0, r.left + r.width, y}, b{r.left, y, r.left + r.width, r.height};
            gpu.list->ClearRenderTargetView(rtv, colors[focus][0], 1, &a);
            gpu.list->ClearRenderTargetView(rtv, colors[focus][1], 1, &b);
        }
    }
    gpu.flush();
    layerReleaseSwapchainImage(chain, nullptr);

    XrCompositionLayerProjectionView pv[2];
    for (int e = 0; e < 2; ++e) {
        pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        pv[e].pose = views[e].pose;
        pv[e].fov = views[e].fov;  // ACE submits the located FOV, though it rendered another
        pv[e].subImage = {chain, {{static_cast<int32_t>(e * eyeW), 0}, {static_cast<int32_t>(eyeW),
                                                                        static_cast<int32_t>(eyeH)}}, 0};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.viewCount = 2;
    layer.views = pv;
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.layerCount = 1;
    end.layers = layers;
    return layerEndFrame(nullptr, &end);
}

std::vector<uint8_t> read_back(ID3D12Resource* image, UINT& pitch) {
    const D3D12_RESOURCE_DESC d = image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC b = {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = total;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&buffer));
    D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    barrier.Transition = {image, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    gpu.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    gpu.list->ResourceBarrier(1, &barrier);
    gpu.flush();
    std::vector<uint8_t> data(static_cast<size_t>(total));
    void* mapped = nullptr;
    buffer->Map(0, nullptr, &mapped);
    std::memcpy(data.data(), mapped, data.size());
    buffer->Unmap(0, nullptr);
    pitch = fp.Footprint.RowPitch;
    return data;
}

}  // namespace

int main() {
    using namespace aceqv;
    game = &games::adapter_for(&ace::kGame);  // views4 below is ACE's
    // The log goes to a file of the test's own (opened at its first line); the pacing check reads it back.
    wchar_t logDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, logDir);
    const std::wstring logPath = std::wstring(logDir) + L"aceqv-quad-flow-test.log";
    DeleteFileW(logPath.c_str());
    SetEnvironmentVariableW(L"ACE_QV_LOG", logPath.c_str());
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)))) {
        std::printf("FAIL: no WARP device\n");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&gpu.queue));
    gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&gpu.list));
    gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpu.rtvHeap));

    // The launcher's runtime report (ACE_QV_RUNTIME_INFO), read once by the layer.
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring runtimeInfo = std::wstring(tempDir) + L"aceqv-quad-flow-runtime.ini";
    DeleteFileW(runtimeInfo.c_str());
    SetEnvironmentVariableW(L"ACE_QV_RUNTIME_INFO", runtimeInfo.c_str());
    next.EnumerateViewConfigurationViews = fakeViews;
    next.CreateSwapchain = fakeCreateSwapchain;
    next.CreateReferenceSpace = fakeCreateReferenceSpace;
    next.DestroySpace = fakeDestroySpace;
    next.DestroySwapchain = fakeDestroySwapchain;
    next.EnumerateSwapchainImages = fakeEnumerateImages;
    next.AcquireSwapchainImage = fakeAcquire;
    next.WaitSwapchainImage = fakeWaitImage;
    next.ReleaseSwapchainImage = fakeRelease;
    next.WaitFrame = fakeWait;
    next.BeginFrame = fakeBegin;
    next.EndFrame = fakeEnd;
    next.LocateViews = fakeLocate;
    next.StringToPath = fakeStringToPath;
    next.CreateActionSet = fakeCreateActionSet;
    next.CreateAction = fakeCreateAction;
    next.SuggestInteractionProfileBindings = fakeSuggest;
    next.AttachSessionActionSets = fakeAttach;
    next.SyncActions = fakeSync;
    next.CreateActionSpace = fakeCreateActionSpace;
    next.LocateSpace = fakeLocateSpace;
    experiment.quad = experiment.twoLoops = experiment.focusInB = experiment.gaze = true;
    experiment.eyeScale = 0.5;
    experiment.focusFraction = 0.35;
    experiment.feather = 0.0;
    quad.device = gpu.device.Get();
    quad.queue = gpu.queue.Get();
    // ACE creates a short-lived instance before its real one: only the real one's actions may be suggested.
    gaze_init(reinterpret_cast<XrInstance>(1));
    const XrAction realGaze = reinterpret_cast<XrAction>(input.nextAction);
    gaze_init(reinterpret_cast<XrInstance>(2));

    // ACE's input setup: its own eye gaze binding, then its one attach.
    XrPath profile = XR_NULL_PATH, posePath = XR_NULL_PATH;
    fakeStringToPath(nullptr, "/interaction_profiles/ext/eye_gaze_interaction", &profile);
    fakeStringToPath(nullptr, "/user/eyes_ext/input/gaze_ext/pose", &posePath);
    const XrActionSuggestedBinding aceBinding{kAceAction, posePath};
    XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggest.interactionProfile = profile;
    suggest.countSuggestedBindings = 1;
    suggest.suggestedBindings = &aceBinding;
    layerSuggestInteractionProfileBindings(nullptr, &suggest);
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &kAceSet;
    const XrResult attachResult = layerAttachSessionActionSets(nullptr, &attach);

    // ACE's start-up: recommended size (halved by eye_scale), then its double-wide eye swapchain, scaled
    // by ACE's own VR resolution setting (a tester's ACE made 4412x1771 from 1297x1042 offered).
    XrViewConfigurationView cfg[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t count = 0;
    layerEnumerateViewConfigurationViews(nullptr, 0, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, cfg);
    const uint32_t offeredW = cfg[0].recommendedImageRectWidth, offeredH = cfg[0].recommendedImageRectHeight;
    const uint32_t eyeW = offeredW * 17 / 10, eyeH = offeredH * 17 / 10;
    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = kFormat;
    sci.sampleCount = sci.faceCount = sci.arraySize = sci.mipCount = 1;
    sci.width = 2 * eyeW;
    sci.height = eyeH;
    XrSwapchain aceChain = XR_NULL_HANDLE;
    layerCreateSwapchain(nullptr, &sci, &aceChain);
    const bool scaledAdopted = quad.appChain == aceChain;
    XrSwapchainCreateInfo ui = sci;  // ACE's 1920x1080 UI chain: never the eye target
    ui.width = 1920;
    ui.height = 1080;
    XrSwapchain uiChain = XR_NULL_HANDLE;
    layerCreateSwapchain(nullptr, &ui, &uiChain);
    XrSwapchainImageD3D12KHR images[3] = {};
    for (auto& i : images)
        i.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
    layerEnumerateSwapchainImages(aceChain, 3, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images));

    bool ok = true;
    auto check = [&](bool condition, const char* what) {
        std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
        ok &= condition;
    };
    check(offeredW == kNativeW / 2 && offeredH == kNativeH / 2, "ACE is offered the eye_scale size");
    check(GetPrivateProfileIntW(L"runtime", L"recommended_width", 0, runtimeInfo.c_str()) == kNativeW &&
              GetPrivateProfileIntW(L"runtime", L"recommended_height", 0, runtimeInfo.c_str()) == kNativeH,
          "the runtime's recommended size is reported for the launcher (ACE_QV_RUNTIME_INFO)");
    check(scaledAdopted && quad.outChain != XR_NULL_HANDLE && quad.appChain == aceChain,
          "output swapchain created for ACE's chain, scaled by ACE's own resolution setting");
    check(input.suggestedBindings == 2, "gaze binding added to ACE's eye gaze suggestion");
    check(input.suggestedActions.size() == 2 && input.suggestedActions[1] == realGaze,
          "only the second instance's gaze action is suggested (none left from the first instance)");
    check(XR_SUCCEEDED(attachResult) && input.attachedSets == 2, "gaze action set attached with ACE's");

    int endErrors = 0;
    for (int i = 0; i < 130; ++i)
        endErrors += XR_FAILED(ace_frame(aceChain, eyeW, eyeH)) ? 1 : 0;
    std::printf("runtime waits=%d begins=%d ends=%d violations=%d endErrors=%d composed=%llu fallbacks=%llu\n",
                rt.waits, rt.begins, rt.ends, rt.violations, endErrors,
                static_cast<unsigned long long>(quad.composed), static_cast<unsigned long long>(quad.fallbacks));
    check(rt.waits == 80 && rt.begins == 80 && rt.ends == 80 && rt.violations == 0 && endErrors == 0,
          "runtime frame and swapchain rules held (30 single + 50 paired frames)");
    check(quad.composed == 50 && quad.fallbacks == 0, "every paired frame composed");
    check(input.syncs == 130 && input.syncedSets == 2, "gaze action set synced with ACE's, once per ACE frame");
    check(gaze.queries == 50 && gaze.tracked == 50, "gaze located once per frame B");
    check(rt.submittedChain == quad.outChain, "submitted image is the layer's output");
    const XrRect2Di& r1 = rt.submittedViews[1].subImage.imageRect;
    check(r1.offset.x == static_cast<int32_t>(kNativeW) && r1.extent.width == static_cast<int32_t>(kNativeW) &&
              r1.extent.height == static_cast<int32_t>(kNativeH),
          "submitted rects are native-size eye halves");
    check(rt.submittedViews[0].fov.angleLeft == kFullFov.angleLeft &&
              rt.submittedViews[0].fov.angleDown == kFullFov.angleDown,
          "submitted FOV is the full FOV");

    // Pixels: the focus box is centred on the gaze; straight ahead is now outside it.
    UINT pitch = 0;
    FakeChain* out = &output_chain(quad.outChain, "quad pixels");
    const auto px = read_back(out->images[rt.submittedImage].Get(), pitch);
    auto at = [&](int eye, double u, double v) {
        const UINT x = static_cast<UINT>((eye + u) * kNativeW), y = static_cast<UINT>(v * kNativeH);
        return &px[y * pitch + x * 4];
    };
    const double l = std::tan(kFullFov.angleLeft), r = std::tan(kFullFov.angleRight);
    const double u = std::tan(kFullFov.angleUp), d = std::tan(kFullFov.angleDown);
    const double ahead_u = -l / (r - l), ahead_v = u / (u - d);
    const double gaze_u = (kGazeX - l) / (r - l), gaze_v = (u - kGazeY) / (u - d);
    for (int eye = 0; eye < 2; ++eye) {
        const uint8_t* g = at(eye, gaze_u, gaze_v);
        const uint8_t* ahead = at(eye, ahead_u, ahead_v);
        std::printf("     eye %d gaze (%.2f,%.2f) = %u,%u,%u, straight ahead (%.2f,%.2f) = %u,%u,%u\n", eye, gaze_u, gaze_v,
                    g[0], g[1], g[2], ahead_u, ahead_v, ahead[0], ahead[1], ahead[2]);
        check(g[0] == 0 && g[1] == 255 && ahead[0] == 255 && ahead[1] == 0, "focus box follows the gaze");
    }

    // views4 with unequal views: periphery 50% of native, focus views 25% of the native eye size.
    // The layer must offer an eye of 0.75 x 0.5 native, compose outer = view e and focus = view e + 2
    // from wherever the layout puts them (never the unused rows), and without composition submit
    // views 0 and 1 at their own size.
    {
        experiment.quad = experiment.twoLoops = experiment.focusInB = experiment.gaze = false;
        experiment.views4 = experiment.views4Compose = true;
        experiment.views4Layout = views4_layout(0.5, 0.25);
        experiment.focusFraction = 0.35;
        experiment.feather = 0.0;
        {
            std::lock_guard guard(quad.mutex);
            quad.compositor.shutdown();
            quad.appChain = quad.outChain = XR_NULL_HANDLE;
            quad.appImages.clear();
            quad.outImages.clear();
            quad.composed = quad.fallbacks = quad.fallbackRun = 0;
            quad.v4Composed = quad.v4FovValid = false;
        }
        layerEnumerateViewConfigurationViews(nullptr, 0, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, cfg);
        const uint32_t w4 = cfg[0].recommendedImageRectWidth, h4 = cfg[0].recommendedImageRectHeight;
        std::printf("     views4 offered %ux%u per eye\n", w4, h4);
        check(w4 == kNativeW * 3 / 4 && h4 == kNativeH / 2, "views4 offers outer + focus wide, the taller one high");
        // ACE scales what it was offered by its own resolution setting (x1.5 here).
        const uint32_t v4EyeW = w4 * 3 / 2, v4EyeH = h4 * 3 / 2;
        XrSwapchainCreateInfo v4ci = sci;
        v4ci.width = 2 * v4EyeW;
        v4ci.height = v4EyeH;
        XrSwapchain v4Chain = XR_NULL_HANDLE;
        layerCreateSwapchain(nullptr, &v4ci, &v4Chain);
        layerEnumerateSwapchainImages(v4Chain, 3, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images));
        check(quad.appChain == v4Chain && quad.outChain != XR_NULL_HANDLE, "views4 adopts ACE's four-view chain");

        int v4Errors = 0;
        for (int i = 0; i < 3; ++i)
            v4Errors += XR_FAILED(ace_views4_frame(v4Chain, v4EyeW, v4EyeH)) ? 1 : 0;
        check(v4Errors == 0 && rt.violations == 0 && quad.composed == 3 && quad.fallbacks == 0,
              "views4: every frame composed, runtime rules held");
        check(rt.submittedChain == quad.outChain && rt.submittedViews[1].subImage.imageRect.extent.width ==
                                                        static_cast<int32_t>(kNativeW),
              "views4 submits the native-size output");
        FakeChain* v4Out = &output_chain(quad.outChain, "views4 pixels");
        const auto px4 = read_back(v4Out->images[rt.submittedImage].Get(), pitch);
        auto at4 = [&](int eye, double uu, double vv) {
            const UINT x = static_cast<UINT>((eye + uu) * kNativeW), y = static_cast<UINT>(vv * kNativeH);
            return &px4[y * pitch + x * 4];
        };
        // The focus box: focus_percent of the tangent extent, centred on the forward axis.
        const double hw = (r - l) * experiment.focusFraction / 2, hh = (u - d) * experiment.focusFraction / 2;
        const double boxL = (-hw - l) / (r - l), boxR = (hw - l) / (r - l);
        const double boxT = (u - hh) / (u - d), boxB = (u + hh) / (u - d);
        const uint8_t outerRgb[2][3] = {{255, 0, 0}, {0, 0, 255}}, focusRgb[2][3] = {{0, 255, 0}, {255, 255, 0}};
        for (int eye = 0; eye < 2; ++eye) {
            const struct {
                double uu, vv;
                bool focus;
                const char* where;
            } probes[] = {
                {0.03, 0.03, false, "top left"},           {0.98, 0.5, false, "right edge"},
                {0.02, 0.5, false, "left edge"},           {0.5, 0.98, false, "bottom edge"},
                {ahead_u, ahead_v, true, "box centre"},    {ahead_u, boxB - 0.02, true, "box bottom"},
                {boxL + 0.02, ahead_v, true, "box left"},  {boxR - 0.02, boxB - 0.02, true, "box bottom right"},
            };
            bool all = true;
            for (const auto& pr : probes) {
                const uint8_t* c = at4(eye, pr.uu, pr.vv);
                const uint8_t* want = pr.focus ? focusRgb[eye] : outerRgb[eye];
                const bool good = c[0] == want[0] && c[1] == want[1] && c[2] == want[2];
                if (!good)
                    std::printf("     eye %d %s (%.2f,%.2f) = %u,%u,%u, want %u,%u,%u\n", eye, pr.where, pr.uu, pr.vv, c[0],
                                c[1], c[2], want[0], want[1], want[2]);
                all &= good;
            }
            (void)boxT;
            check(all, eye ? "views4 eye 1: blue outer view, yellow focus view, no neighbour or unused rows"
                           : "views4 eye 0: red outer view, green focus view, no neighbour or unused rows");
        }

        // Headset-agnostic alignment: ACE renders a FOV other than the located one (vertically
        // asymmetric, and different every frame). The composition must use the frame's record (what
        // was rendered), submit the output with that FOV, and line the focus view up with the outer
        // view on both axes.
        {
            const float rendered[2][4] = {{-1.00f, 0.90f, 0.70f, -1.10f}, {-1.05f, 0.95f, 0.74f, -1.16f}};
            const struct {
                bool vertical;
                float split;
            } splits[] = {{true, 0.15f}, {false, -0.12f}};
            bool aligned = true, submittedRendered = true;
            for (int f = 0; f < 2; ++f) {
                Views4Frame frame{};
                for (int e = 0; e < 2; ++e) {
                    std::memcpy(frame.outerTan[e], rendered[f], sizeof(rendered[f]));
                    double hw, hh;
                    focus_extents(rendered[f], experiment.focusFraction, hw, hh);
                    const float focusTan[4] = {static_cast<float>(-hw), static_cast<float>(hw), static_cast<float>(hh),
                                               static_cast<float>(-hh)};
                    std::memcpy(frame.focusTan[e], focusTan, sizeof(focusTan));
                    frame.turn[e] = {0, 0, 0, 1};
                }
                const auto& sp = splits[f];
                if (XR_FAILED(ace_views4_split_frame(v4Chain, v4EyeW, v4EyeH, frame, sp.vertical, sp.split)))
                    aligned = false;
                for (int e = 0; e < 2; ++e) {
                    const XrFovf& sub = rt.submittedViews[e].fov;
                    const float* t = rendered[f];
                    submittedRendered &= std::fabs(std::tan(sub.angleLeft) - t[0]) < 1e-4f &&
                                         std::fabs(std::tan(sub.angleRight) - t[1]) < 1e-4f &&
                                         std::fabs(std::tan(sub.angleUp) - t[2]) < 1e-4f &&
                                         std::fabs(std::tan(sub.angleDown) - t[3]) < 1e-4f;
                }
                const auto pxs = read_back(output_chain(quad.outChain, "views4 FOV pixels").images[rt.submittedImage].Get(), pitch);
                const float* t = rendered[f];
                auto colour = [&](int eye, double tx, double ty) {
                    const double uu = (tx - t[0]) / (t[1] - t[0]), vv = (t[2] - ty) / (t[2] - t[3]);
                    const UINT x = static_cast<UINT>((eye + uu) * kNativeW), y = static_cast<UINT>(vv * kNativeH);
                    return &pxs[y * pitch + x * 4];
                };
                constexpr double kStep = 0.08;
                for (int e = 0; e < 2; ++e) {
                    // (tangent x, tangent y, want): just before and after the split, inside the box
                    // (focus colours) and outside it (outer colours).
                    struct Probe {
                        double tx, ty;
                        uint8_t r, g, b;
                    };
                    const Probe vertical[] = {{sp.split - kStep, 0.0, 0, 255, 0},
                                              {sp.split + kStep, 0.0, 255, 255, 0},
                                              {sp.split - kStep, 0.55, 255, 0, 0},
                                              {sp.split + kStep, 0.55, 0, 0, 255}};
                    const Probe horizontal[] = {{0.0, sp.split + kStep, 0, 255, 0},
                                                {0.0, sp.split - kStep, 255, 255, 0},
                                                {-0.7, sp.split + kStep, 255, 0, 0},
                                                {-0.7, sp.split - kStep, 0, 0, 255}};
                    const Probe* list = sp.vertical ? vertical : horizontal;
                    for (int k = 0; k < 4; ++k) {
                        const Probe& pr = list[k];
                        const uint8_t* c = colour(e, pr.tx, pr.ty);
                        const bool good = c[0] == pr.r && c[1] == pr.g && c[2] == pr.b;
                        if (!good)
                            std::printf("     frame %d eye %d tangent (%.2f,%.2f) = %u,%u,%u, want %u,%u,%u\n", f, e, pr.tx,
                                        pr.ty, c[0], c[1], c[2], pr.r, pr.g, pr.b);
                        aligned &= good;
                    }
                }
            }
            check(submittedRendered, "views4 submits the output with the FOV ACE rendered, not the one it submitted");
            check(aligned, "views4 focus and outer views line up on both axes when ACE renders a FOV of its own");
        }

        // Without composition ACE's own image goes out: views 0 and 1 at their own size.
        experiment.views4Compose = false;
        ace_views4_frame(v4Chain, v4EyeW, v4EyeH);
        const XrRect2Di& s0 = rt.submittedViews[0].subImage.imageRect;
        const XrRect2Di& s1 = rt.submittedViews[1].subImage.imageRect;
        std::printf("     views4 uncomposed rects %d,%d %dx%d and %d,%d %dx%d\n", s0.offset.x, s0.offset.y,
                    s0.extent.width, s0.extent.height, s1.offset.x, s1.offset.y, s1.extent.width, s1.extent.height);
        const ViewRect e1 = views4_rect(experiment.views4Layout, 1, static_cast<int>(2 * v4EyeW), static_cast<int>(v4EyeH));
        check(rt.submittedChain == v4Chain && s0.offset.x == 0 && s0.extent.width == e1.width &&
                  s1.offset.x == e1.left && s1.extent.width == e1.width && s1.extent.height == static_cast<int32_t>(v4EyeH) &&
                  e1.width == static_cast<int>(2 * v4EyeW) / 3,
              "views4 uncomposed: views 0 and 1 submitted at a third of the target each");

        // A compositor that cannot start (a shader that does not compile, say): no output chain, and ACE's
        // own image goes out, every frame, with nothing failing.
        experiment.views4Compose = true;
        {
            std::lock_guard guard(quad.mutex);
            quad.compositor.shutdown();
            quad.appChain = quad.outChain = XR_NULL_HANDLE;
            quad.appImages.clear();
            quad.outImages.clear();
            quad.composed = quad.fallbacks = quad.fallbackRun = 0;
            quad.v4Composed = quad.v4FovValid = false;
        }
        QuadCompositor::failInitialize = true;
        XrSwapchain broken = XR_NULL_HANDLE;
        layerCreateSwapchain(nullptr, &v4ci, &broken);
        layerEnumerateSwapchainImages(broken, 3, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images));
        int brokenErrors = 0;
        for (int i = 0; i < 3; ++i)
            brokenErrors += XR_FAILED(ace_views4_frame(broken, v4EyeW, v4EyeH)) ? 1 : 0;
        QuadCompositor::failInitialize = false;
        check(quad.outChain == XR_NULL_HANDLE && brokenErrors == 0 && rt.violations == 0 && quad.composed == 0 &&
                  rt.submittedChain == broken,
              "views4 without a compositor: ACE's own image is submitted, nothing fails");

        // The helmet's pieces in their own head-locked layer, with the crop: the scene keeps the cropped FOV
        // ACE renders (so frame generation's depth and motion match it), the quad goes right after it, in VIEW
        // space, at the helmet's distance, its swapchain image released.
        {
            std::lock_guard guard(quad.mutex);
            quad.compositor.shutdown();
            quad.appChain = quad.outChain = XR_NULL_HANDLE;
            quad.appImages.clear();
            quad.outImages.clear();
            quad.composed = quad.fallbacks = quad.fallbackRun = 0;
            quad.v4Composed = quad.v4FovValid = false;
            for (int p = 0; p < 3; ++p)
                quad.helmetPiecesTried[p].clear();
        }
        const std::wstring brow = std::wstring(tempDir) + L"aceqv-flow-top.png";
        write_file_png(brow);
        helmetSettings = HelmetSettings{};
        helmetSettings.on = true;
        helmetSettings.topImage = brow;
        helmetSettings.bottomImage = helmetSettings.visorImage = L"";
        helmetSettings.depth = 8;
        experiment.set_crop(fov_crop(20, 20, 0, 0));
        XrSwapchain helmeted = XR_NULL_HANDLE;
        layerCreateSwapchain(nullptr, &v4ci, &helmeted);
        layerEnumerateSwapchainImages(helmeted, 3, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images));
        int helmetErrors = 0;
        for (int i = 0; i < 4; ++i)
            helmetErrors += XR_FAILED(ace_views4_frame(helmeted, v4EyeW, v4EyeH)) ? 1 : 0;
        const XrRect2Di& scene = rt.submittedViews[0].subImage.imageRect;
        const XrCompositionLayerQuad& hq = rt.submittedQuad;
        std::printf("     helmet: scene %dx%d (native %ux%u), quad at layer %d (scene %d), %.3f x %.3f m, %.3f m ahead\n",
                    scene.extent.width, scene.extent.height, kNativeW, kNativeH, rt.submittedQuadAt,
                    rt.submittedProjectionAt, hq.size.width, hq.size.height, -hq.pose.position.z);
        check(helmetErrors == 0 && rt.violations == 0 && quad.helmetChain != XR_NULL_HANDLE &&
                  rt.submittedQuadAt == rt.submittedProjectionAt + 1 && hq.subImage.swapchain == quad.helmetChain &&
                  hq.space == quad.helmetSpace && std::fabs(hq.pose.position.z + 0.08f) < 1e-4f && hq.size.width > 0.16f &&
                  (hq.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) &&
                  !(hq.layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT),
              "helmet: its own premultiplied quad layer over the scene, 8 cm ahead");
        check(scene.extent.height == static_cast<int32_t>(std::lround(kNativeH * 0.6)) &&
                  std::tan(rt.submittedViews[0].fov.angleUp) < std::tan(kFullFov.angleUp) - 0.01f,
              "helmet: the scene is submitted cropped, not over the whole eye");
        helmetSettings = HelmetSettings{};
        experiment.set_crop(FovCrop{});
        DeleteFileW(brow.c_str());
    }
    // The pacing line: frames sorted by the display periods they took (a 22.5 ms frame is two of 11.1 ms).
    {
        pacing = Pacing{};
        pacing.periodMs = 11.1;
        pacing.interval = {11.0f, 22.5f, 22.0f, 33.4f, 50.0f};
        pacing.wait = {5.0f, 10.0f, 10.0f, 2.0f, 1.0f};
        report_pacing();
        std::string logged;
        if (FILE* f = _wfopen(logPath.c_str(), L"rb")) {
            char buffer[4096];
            for (size_t n; (n = fread(buffer, 1, sizeof(buffer), f)) > 0;)
                logged.append(buffer, n);
            fclose(f);
        }
        const size_t at = logged.rfind("views4 pacing:");
        const std::string line = at == std::string::npos ? "" : logged.substr(at, logged.find('\n', at) - at);
        std::printf("     %s\n", line.c_str());
        check(line.find("frames of 1/2/3/4+ display periods 1/2/1/1") != std::string::npos &&
                  line.find("display period 11.10 ms") != std::string::npos && pacing.interval.empty(),
              "pacing: frames counted by the display periods they took, then reset");
        // The load line: ACE's threads sampled (a second's worth), reported once, the window emptied.
        cpu_load_start();
        Sleep(1200);
        cpu_load_stop();  // no sample lands after the report
        report_load(22.2);
        std::string after;
        if (FILE* f = _wfopen(logPath.c_str(), L"rb")) {
            char buffer[4096];
            for (size_t n; (n = fread(buffer, 1, sizeof(buffer), f)) > 0;)
                after.append(buffer, n);
            fclose(f);
        }
        const size_t loadAt = after.rfind("views4 load:");
        const std::string load = loadAt == std::string::npos ? "" : after.substr(loadAt, after.find('\n', loadAt) - loadAt);
        std::printf("     %s\n", load.c_str());
        int left = 0;
        cpu_load_window(left);
        check(load.find("busiest ACE thread") != std::string::npos && load.find("GPU load unknown") != std::string::npos &&
                  left == 0,
              "load: the threads' line, the GPU unknown without its sampler; the window emptied");
    }
    {
        // Frame records: filled and composed in lockstep, each composition gets its own frame's record. Two
        // records filled and never composed (2026-11-02, the focus lagging the eyes) put every composition two
        // frames behind; after a while of that the queue resyncs to the newest record.
        using namespace aceqv;
        const auto marked = [](float marker) {
            Views4Frame f{};
            f.outerTan[0][0] = marker;
            return f;
        };
        Views4Frame taken{};
        for (int i = 0; i < 200; ++i) {  // whatever earlier checks left: until a composition gets its own record
            views4_push_frame(marked(-1.0f - i));
            views4_take_frame(taken);
            if (taken.outerTan[0][0] == -1.0f - i)
                break;
        }
        bool lockstep = true;
        for (int i = 0; i < 200; ++i) {
            views4_push_frame(marked(1000.0f + i));
            views4_take_frame(taken);
            lockstep &= taken.outerTan[0][0] == 1000.0f + i;
        }
        check(lockstep, "frame records: filled and composed in lockstep, every composition gets its own frame's");
        views4_push_frame(marked(1.0f));  // filled, never composed
        views4_push_frame(marked(2.0f));
        int behindFrames = 0, firstOwn = -1;
        for (int i = 0; i < 200; ++i) {
            views4_push_frame(marked(5000.0f + i));
            views4_take_frame(taken);
            if (taken.outerTan[0][0] == 5000.0f + i) {
                if (firstOwn < 0)
                    firstOwn = i;
            } else if (firstOwn < 0 && taken.outerTan[0][0] == (i >= 2 ? 5000.0f + i - 2 : static_cast<float>(i + 1))) {
                ++behindFrames;
            }
        }
        std::printf("     two frames behind for %d compositions, own records from composition %d\n", behindFrames, firstOwn);
        check(firstOwn > 0 && firstOwn <= 100 && behindFrames == firstOwn,
              "frame records: two filled and never composed are made up within the drift window, not kept for good");
        bool stays = true;
        for (int i = 0; i < 200; ++i) {
            views4_push_frame(marked(9000.0f + i));
            views4_take_frame(taken);
            stays &= taken.outerTan[0][0] == 9000.0f + i;
        }
        check(stays, "frame records: in lockstep again afterwards");
    }
    {
        // The gaze thread (views4): ACE's xrLocateViews reads the latest gaze, the thread looks it up at its own
        // rate. Needs the gaze action attached and ACE syncing actions, as the quad frames above left it.
        bool ready;
        {
            std::lock_guard guard(gaze.mutex);
            ready = gaze.attached && gaze.appSyncs > 0;
        }
        check(ready, "gaze thread: the gaze action attached and ACE syncing (the precondition)");
        experiment.gazeThread = true;
        experiment.gazePollHz = 40;  // slow: lookups follow the thread's rate, not the calls
        const XrSpace base = reinterpret_cast<XrSpace>(0x200);
        XrVector3f d{};
        gaze_now(nullptr, base, 1000, d);  // starts the thread
        Sleep(100);
        uint64_t before;
        {
            std::lock_guard guard(gaze.mutex);
            before = gaze.queries;
        }
        bool got = true;
        for (int i = 0; i < 100; ++i)
            got &= gaze_now(nullptr, base, 1000 + i, d);
        uint64_t after;
        {
            std::lock_guard guard(gaze.mutex);
            after = gaze.queries;
        }
        const double len = std::sqrt(kGazeX * kGazeX + kGazeY * kGazeY + 1);
        std::printf("     gaze thread: %llu lookups during 100 calls, direction %.3f,%.3f,%.3f\n",
                    static_cast<unsigned long long>(after - before), d.x, d.y, d.z);
        check(got && std::abs(d.x - kGazeX / len) < 1e-3 && std::abs(d.y - kGazeY / len) < 1e-3,
              "gaze thread: xrLocateViews gets the tracked gaze the thread looked up");
        check(after - before <= 3, "gaze thread: 100 xrLocateViews calls look nothing up themselves");
        {
            std::lock_guard guard(gaze.mutex);
            gaze.appSyncs = 0;
        }
        const uint64_t inlineBefore = gaze.queries;
        gaze_now(nullptr, base, 2000, d);
        check(gaze.queries == inlineBefore + 1, "gaze thread: without ACE syncing actions, looked up inline as before");
        gaze_thread_stop();
        check(!gazeThread.thread.joinable(), "gaze thread: stops with the session");
    }
    DeleteFileW(logPath.c_str());
    std::printf("RESULT: %s\n", ok ? "pass" : "FAIL");
    return ok ? 0 : 1;
}
