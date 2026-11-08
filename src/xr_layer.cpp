// XR_APILAYER_SIM_qvfr: logs what the game asks for (views, swapchains, frame submission) and starts the
// log-only engine probes (probe/probe.cpp) at xrCreateSession. With mode=off nothing alters rendering; the
// experiments and the quad pipeline are selected in sim-qvfr.ini. This file has the loader side:
// GetInstanceProcAddr, the layer order and gaze extension checks, CreateApiLayerInstance and the exported
// negotiation; the rest is in src/layer/ (layer.h).
#include <cstring>
#include <string>
#include <vector>

#include "cheeky/cheeky_host.h"
#include "games/adapter.h"
#include "games/game.h"
#include "layer_chain.h"
#include "log.h"
#include "probe/dred.h"
#include "probe/pix_capture.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

#define SIMQVFR_INTERCEPT(fn)                                              \
    if (std::strcmp(name, "xr" #fn) == 0) {                              \
        *function = reinterpret_cast<PFN_xrVoidFunction>(&layer##fn);    \
        return XR_SUCCESS;                                               \
    }

XrResult XRAPI_CALL layerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function)
        return XR_ERROR_VALIDATION_FAILURE;
    SIMQVFR_INTERCEPT(GetInstanceProcAddr)
    if (next.EndFrame) {
        SIMQVFR_INTERCEPT(EnumerateViewConfigurationViews)
        SIMQVFR_INTERCEPT(CreateSession)
        SIMQVFR_INTERCEPT(DestroySession)
        SIMQVFR_INTERCEPT(BeginSession)
        SIMQVFR_INTERCEPT(WaitFrame)
        SIMQVFR_INTERCEPT(BeginFrame)
        SIMQVFR_INTERCEPT(CreateSwapchain)
        SIMQVFR_INTERCEPT(EnumerateSwapchainImages)
        SIMQVFR_INTERCEPT(LocateViews)
        SIMQVFR_INTERCEPT(CreateReferenceSpace)
        SIMQVFR_INTERCEPT(DestroySpace)
        SIMQVFR_INTERCEPT(EndFrame)
        SIMQVFR_INTERCEPT(AcquireSwapchainImage)
        SIMQVFR_INTERCEPT(ReleaseSwapchainImage)
        SIMQVFR_INTERCEPT(SuggestInteractionProfileBindings)
        SIMQVFR_INTERCEPT(AttachSessionActionSets)
        SIMQVFR_INTERCEPT(SyncActions)
        SIMQVFR_INTERCEPT(PollEvent)
        if (next.GetVisibilityMaskKHR)
            SIMQVFR_INTERCEPT(GetVisibilityMaskKHR)
    }
    return next.GetInstanceProcAddr ? next.GetInstanceProcAddr(instance, name, function) : XR_ERROR_HANDLE_INVALID;
}

namespace {

// The layers between this one and the runtime. VectorXR's frame generation has to be among them:
// above this layer it would get ACE's four-view image instead of the composed stereo frame.
void log_layer_order(const XrApiLayerNextInfo* below) {
    std::string names;
    bool vectorxrBelow = false;
    for (const XrApiLayerNextInfo* n = below; n; n = n->next) {
        names += names.empty() ? "" : ", ";
        names += n->layerName;
        vectorxrBelow |= std::strstr(n->layerName, "VECTORXR") != nullptr;
    }
    logf("layers below this one: [%s]", names.c_str());
    layersBelow = names;
    if (!vectorxrBelow && GetModuleHandleW(L"XR_APILAYER_DIENERTECH_VECTORXR.dll"))
        log_line("VectorXR is loaded above this layer: its frame generation would see the game's four-view image; move "
                 "this layer above VectorXR (machine-wide registration, then VectorXR's layer order)");
}

// Whether the layer adds XR_EXT_eye_gaze_interaction to the game's instance (layerCreateApiLayerInstance).
enum class GazeExtension {
    Requested,  // the game enables it itself
    Add,        // the runtime (through the layers below) lists it: add it
    Try,        // the list didn't answer: add it, and create the instance without it if that fails
    Absent,     // the runtime doesn't list it: leave the request as it is
};

GazeExtension gaze_extension(const XrInstanceCreateInfo& info, PFN_xrGetInstanceProcAddr gipa) {
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
        if (std::strcmp(info.enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            return GazeExtension::Requested;
    PFN_xrVoidFunction function = nullptr;
    if (!gipa || XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function)) || !function)
        return GazeExtension::Try;
    const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(function);
    uint32_t count = 0;
    if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr)))
        return GazeExtension::Try;
    std::vector<XrExtensionProperties> properties(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (count && XR_FAILED(enumerate(nullptr, count, &count, properties.data())))
        return GazeExtension::Try;
    for (const XrExtensionProperties& p : properties)
        if (std::strcmp(p.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            return GazeExtension::Add;
    return GazeExtension::Absent;
}

// Why a game can start flat with this layer loaded: the OpenXR loader negotiates every layer, then can still
// fail xrCreateInstance before the chain reaches this one (a layer named in XR_ENABLE_API_LAYERS whose manifest
// it refuses, an extension nobody provides). The loader prints its reason on the game's stderr, which the launcher
// keeps in the run folder's openxr-loader.log; this log names the layers and says when that happened.
void note_loader_negotiation() {
    const int n = ++loaderNegotiations;
    if (n == 1) {
        char enabled[2048] = {};
        GetEnvironmentVariableA("XR_ENABLE_API_LAYERS", enabled, static_cast<DWORD>(std::size(enabled)));
        logf("OpenXR layers named for this launch: %s", enabled[0] ? enabled : "(none: implicit layers only)");
    } else if (!instanceReachedLayer) {
        log_line("the game's previous xrCreateInstance failed before reaching this layer (no VR: the game starts "
                 "flat). The OpenXR loader's reason is in this launch's run folder, openxr-loader.log");
    }
}

// The layers named in SIM_QVFR_CHAIN, loaded from their manifests in XR_API_LAYER_PATH and linked below
// `self` (layer_chain.h: the launcher does this for a game whose OpenXR loader can't take a list of layers).
// A layer that fails to load is left out and logged, as the loader would. `self` when there is none.
const XrApiLayerNextInfo* chain_layers_below(const XrApiLayerNextInfo* self) {
    static std::vector<XrApiLayerNextInfo> storage;  // outlives the create call down the chain
    wchar_t value[4096] = {}, folder[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(chain::kChainVariable, value, static_cast<DWORD>(std::size(value)));
    if (n == 0 || n >= std::size(value))
        return self;
    GetEnvironmentVariableW(L"XR_API_LAYER_PATH", folder, static_cast<DWORD>(std::size(folder)));
    std::vector<chain::Layer> layers;
    char names[4096] = {};  // layer names are ASCII
    WideCharToMultiByte(CP_UTF8, 0, value, -1, names, static_cast<int>(std::size(names)), nullptr, nullptr);
    for (const std::string& name : chain::split_names(names)) {
        chain::Layer layer;
        std::string why;
        if (chain::load(folder, name, layer, why)) {
            layers.push_back(layer);
            logf("chain: %s loaded below this layer", name.c_str());
        } else {
            logf("chain: %s left out: %s", name.c_str(), why.c_str());
        }
    }
    return chain::link(layers, self, storage);
}

}  // namespace

XrResult XRAPI_CALL layerCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    if (!info || !layerInfo || !layerInfo->nextInfo || !instance ||
        std::strcmp(layerInfo->nextInfo->layerName, kLayerName) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;
    instanceReachedLayer = true;
    // This layer's node, with the layers the launcher asked it to chain (SIM_QVFR_CHAIN) linked below it.
    const XrApiLayerNextInfo* own = chain_layers_below(layerInfo->nextInfo);
    XrApiLayerCreateInfo nextLayerInfo = *layerInfo;
    nextLayerInfo.nextInfo = own->next;
    PFN_xrGetInstanceProcAddr gipa = own->nextGetInstanceProcAddr;
    // Registered as an implicit layer, so it loads into every OpenXR app: stay silent and
    // pass-through everywhere except a supported game (src/games).
    const games::GameInfo* thisGame = games::current_game();
    if (!thisGame) {
        next = Dispatch{gipa};
        return own->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    }
    // Eye tracking needs XR_EXT_eye_gaze_interaction on the instance. ACE and X-Plane ask for it themselves; ACC
    // (through OpenComposite) doesn't, so its gaze worked only with a layer below that adds it (Cheeky, OpenXR Toolkit,
    // VectorXR), and failed with none (the eye gaze profile's bindings rejected, XR_ERROR_PATH_UNSUPPORTED). So the
    // layer adds it when the game hasn't and the runtime offers it; with the offer unknown it tries, and creates the
    // instance without it if the runtime refuses.
    const GazeExtension gazeExtension = gaze_extension(*info, gipa);
    XrResult r = XR_ERROR_EXTENSION_NOT_PRESENT;
    if (gazeExtension == GazeExtension::Add || gazeExtension == GazeExtension::Try) {
        std::vector<const char*> names(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
        names.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        XrInstanceCreateInfo withGaze = *info;
        withGaze.enabledExtensionCount = static_cast<uint32_t>(names.size());
        withGaze.enabledExtensionNames = names.data();
        r = own->nextCreateApiLayerInstance(&withGaze, &nextLayerInfo, instance);
    }
    const bool gazeAdded = XR_SUCCEEDED(r);
    if (!gazeAdded)
        r = own->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    game = &games::adapter_for(thisGame);
    load_experiment();
    // Where the launcher writes live settings while this ACE runs (live_reload).
    runtime_info(L"live_ini", live.ini);
    runtime_info(L"live_pid", std::to_wstring(GetCurrentProcessId()));
    log_layer_order(own->next);
    logf("xrCreateApiLayerInstance app=%s engine=%s extensions=%u result=%d", info->applicationInfo.applicationName,
         info->applicationInfo.engineName, info->enabledExtensionCount, r);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        logf("  extension %s", info->enabledExtensionNames[i]);
    switch (gazeExtension) {
    case GazeExtension::Requested: log_line("gaze: the game enables XR_EXT_eye_gaze_interaction itself"); break;
    case GazeExtension::Absent: log_line("gaze: the runtime doesn't offer XR_EXT_eye_gaze_interaction (no eye tracking)"); break;
    case GazeExtension::Add:
    case GazeExtension::Try:
        logf("gaze: XR_EXT_eye_gaze_interaction %s (%s)",
             gazeAdded ? "added for the game" : "NOT added: the runtime refused the instance with it",
             gazeExtension == GazeExtension::Add ? "the runtime offers it" : "the runtime's offer unknown, tried");
        break;
    }
    if (XR_FAILED(r))
        return r;
    // Seconds before ACE initialises NGX, so Cheeky's hooks see its DLSS features created. Only in a game
    // it's made for.
    if (game->loads_cheeky()) {
        cheeky_start(cheeky_config(live.ini.c_str(), layer_directory()));
        {  // for the launcher's Foveated DLSS pane
            static const wchar_t* const kStatus[] = {L"off", L"missing", L"refused", L"running"};
            runtime_info(L"cheeky_status", kStatus[static_cast<int>(cheeky_status())]);
            if (cheeky_running())
                runtime_info(L"cheeky_version", cheeky_version());
        }
        cheeky_apply_ini();
    }
    // Before ACE creates its D3D12 device and renderer, so renderer init sees the patched mode.
    if (experiment.dred)
        dred_enable();
    if (experiment.d3d12Debug)
        d3d12_debug_enable(experiment.gpuValidation);
    if (experiment.pixCaptureFrame)  // before ACE creates its D3D12 device
        pix_capture_load(experiment.pixCaptureFrame);
    game->instance_created(live.ini.c_str(), experiment.mode);
    if (experiment.views4) {
        games::QvfrStart start;
        start.layout = experiment.views4Layout;
        start.focusFraction = experiment.views4EyeCamera ? 0.0 : experiment.box_x();
        start.focusFractionY = experiment.box_y();
        start.dlssLive = experiment.views4DlssLive;
        // The cull box: the focus box plus its feather band plus a gaze-latency allowance.
        start.focusCull = experiment.views4FocusCull;
        start.focusCullScale = 1.0 + experiment.feather + experiment.views4FocusCullMargin;
        start.focusCullGaze = experiment.gaze && experiment.composing();  // gaze turns only when composing
        start.ini = live.ini.c_str();
        if (!game->qvfr_start(start)) {
            experiment.views4 = false;  // no doubled eye width or rect rewrite without the engine side
            notice_set(L"Sim-QVFR is off",
                       {L"This " + std::wstring(thisGame->name) +
                            L" build isn’t one Sim-QVFR supports, so the game runs as normal, "
                            L"without foveated rendering. The game has probably been updated.",
                        L"Look for an Sim-QVFR release for the new build. The launcher’s Launch checks say the same "
                        L"before a launch."});
        }
    }
    Dispatch d;
    d.GetInstanceProcAddr = gipa;
    bool ok = true;
#define SIMQVFR_LOAD(fn) ok &= XR_SUCCEEDED(gipa(*instance, "xr" #fn, reinterpret_cast<PFN_xrVoidFunction*>(&d.fn))) && d.fn;
    SIMQVFR_LOAD(EnumerateViewConfigurationViews)
    SIMQVFR_LOAD(CreateSession)
    SIMQVFR_LOAD(DestroySession)
    SIMQVFR_LOAD(BeginSession)
    SIMQVFR_LOAD(WaitFrame)
    SIMQVFR_LOAD(BeginFrame)
    SIMQVFR_LOAD(CreateSwapchain)
    SIMQVFR_LOAD(EnumerateSwapchainImages)
    SIMQVFR_LOAD(LocateViews)
    SIMQVFR_LOAD(EndFrame)
    SIMQVFR_LOAD(AcquireSwapchainImage)
    SIMQVFR_LOAD(WaitSwapchainImage)
    SIMQVFR_LOAD(ReleaseSwapchainImage)
    SIMQVFR_LOAD(StringToPath)
    SIMQVFR_LOAD(CreateActionSet)
    SIMQVFR_LOAD(CreateAction)
    SIMQVFR_LOAD(SuggestInteractionProfileBindings)
    SIMQVFR_LOAD(AttachSessionActionSets)
    SIMQVFR_LOAD(SyncActions)
    SIMQVFR_LOAD(PollEvent)
    SIMQVFR_LOAD(CreateActionSpace)
    SIMQVFR_LOAD(LocateSpace)
    SIMQVFR_LOAD(CreateReferenceSpace)
    SIMQVFR_LOAD(DestroySpace)
    SIMQVFR_LOAD(DestroySwapchain)
    SIMQVFR_LOAD(GetActionStateBoolean)
    SIMQVFR_LOAD(GetActionStateVector2f)
    SIMQVFR_LOAD(GetActionStatePose)
    SIMQVFR_LOAD(GetCurrentInteractionProfile)
    SIMQVFR_LOAD(PathToString)
#undef SIMQVFR_LOAD
    gipa(*instance, "xrGetVisibilityMaskKHR", reinterpret_cast<PFN_xrVoidFunction*>(&d.GetVisibilityMaskKHR));
    if (ok && (experiment.views4 || experiment.measure)) {  // time what runs below this layer (report_cpu_times)
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        cpu.msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
        cpu.realEndFrame = d.EndFrame;
        cpu.realLocateViews = d.LocateViews;
        d.EndFrame = timedEndFrameBelow;
        d.LocateViews = timedLocateViewsBelow;
    }
    next = ok ? d : Dispatch{gipa};
    if (!ok)
        log_line("missing downstream functions; pass-through only");
    else
        gaze_init(*instance);
    return r;
}

}  // namespace simqvfr::layer

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
                                   XrNegotiateApiLayerRequest* request) {
    if (!loaderInfo || !layerName || !request || std::strcmp(layerName, simqvfr::layer::kLayerName) != 0 ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_API_VERSION_1_0;
    request->getInstanceProcAddr = simqvfr::layer::layerGetInstanceProcAddr;
    request->createApiLayerInstance = simqvfr::layer::layerCreateApiLayerInstance;
    if (simqvfr::layer::is_game_process()) {
        simqvfr::log_line("Loader negotiated API layer interface v1 (Sim-QVFR probe build)");
        simqvfr::layer::note_loader_negotiation();
    }
    return XR_SUCCESS;
}
