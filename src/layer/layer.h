// The OpenXR layer's internal header (src/layer/): the state, the configuration, and every function one
// of the layer's files calls in another, including the hooked layer* entry points. The tests include it.
#pragma once

#include <functional>

#include "input/hotkeys.h"
#include "quad/settings_menu.h"
#include "xr.h"
#include "layer/state.h"
#include "layer/config.h"

namespace simqvfr::layer {

// xr_layer.cpp
XrResult XRAPI_CALL layerCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* info, XrSpace* space);
XrResult XRAPI_CALL layerDestroySpace(XrSpace space);
void gaze_init(XrInstance instance);
XrResult XRAPI_CALL layerSuggestInteractionProfileBindings(XrInstance instance,
                                                          const XrInteractionProfileSuggestedBinding* suggested);
XrResult XRAPI_CALL layerAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info);
XrResult XRAPI_CALL layerPollEvent(XrInstance instance, XrEventDataBuffer* event);
XrResult XRAPI_CALL layerSyncActions(XrSession session, const XrActionsSyncInfo* info);
void gaze_thread_stop();
bool gaze_now(XrSession session, XrSpace base, XrTime time, XrVector3f& direction);
int64_t qpc_now();
double report_pacing();
void report_load(double frameMs);
XrResult XRAPI_CALL layerEnumerateViewConfigurationViews(XrInstance instance, XrSystemId system,
                                                         XrViewConfigurationType type, uint32_t capacity,
                                                         uint32_t* countOutput, XrViewConfigurationView* views);
XrResult XRAPI_CALL layerCreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session);
XrResult XRAPI_CALL layerDestroySession(XrSession session);
XrResult XRAPI_CALL layerWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state);
XrResult XRAPI_CALL layerBeginFrame(XrSession session, const XrFrameBeginInfo* info);
XrResult XRAPI_CALL layerBeginSession(XrSession session, const XrSessionBeginInfo* info);
XrResult XRAPI_CALL layerCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain);
XrResult XRAPI_CALL layerEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* countOutput,
                                                  XrSwapchainImageBaseHeader* images);
XrResult XRAPI_CALL layerAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* info,
                                               uint32_t* index);
XrResult XRAPI_CALL layerReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info);
XrResult XRAPI_CALL layerGetVisibilityMaskKHR(XrSession session, XrViewConfigurationType type, uint32_t viewIndex,
                                              XrVisibilityMaskTypeKHR maskType, XrVisibilityMaskKHR* mask);
XrResult XRAPI_CALL layerLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
                                     uint32_t capacity, uint32_t* countOutput, XrView* views);
uint64_t ini_write_later(std::function<void()> write, IniWrite kind = IniWrite::Once);
void ini_worker_sync();
void live_reload();
void hotkeys_changed(const Hotkeys& h);
void menu_values_changed(const IniSnapshot& s);
void menu_press(MenuKey key, int64_t now);
XrResult XRAPI_CALL layerEndFrame(XrSession session, const XrFrameEndInfo* info);
XrResult XRAPI_CALL layerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
XrResult XRAPI_CALL layerCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                const XrApiLayerCreateInfo* layerInfo, XrInstance* instance);

}  // namespace simqvfr::layer
