// The OpenXR layer's internal header (src/layer/): the state, the configuration, and every function one
// of the layer's files calls in another, including the hooked layer* entry points. The tests include it.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cheeky/cheeky_settings.h"
#include "input/hotkeys.h"
#include "quad/compositor.h"
#include "quad/helmet.h"
#include "quad/osd.h"
#include "quad/session_graphics.h"
#include "quad/settings_menu.h"
#include "xr.h"
#include "layer/state.h"
#include "layer/config.h"

namespace simqvfr::layer {

// two_loops.cpp
double ms_between(const LARGE_INTEGER& from, const LARGE_INTEGER& to);
void narrow(float& loAngle, float& hiAngle, double fraction, double centre = 0.0);

// gaze_actions.cpp
bool gaze_skips_space(XrSpace space, XrTime time);
XrResult XRAPI_CALL layerCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* info, XrSpace* space);
XrResult XRAPI_CALL layerDestroySpace(XrSpace space);
XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v);
void gaze_init(XrInstance instance);
std::string path_name(XrInstance instance, XrPath path);
XrResult XRAPI_CALL layerSuggestInteractionProfileBindings(XrInstance instance,
                                                          const XrInteractionProfileSuggestedBinding* suggested);
XrResult XRAPI_CALL layerAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info);
XrResult XRAPI_CALL layerPollEvent(XrInstance instance, XrEventDataBuffer* event);
XrResult XRAPI_CALL layerSyncActions(XrSession session, const XrActionsSyncInfo* info);

// gaze.cpp
bool gaze_direction(XrSession session, XrSpace base, XrTime time, XrVector3f& direction);
void gaze_thread_stop();
bool gaze_now(XrSession session, XrSpace base, XrTime time, XrVector3f& direction);
void steady_gaze(bool tracked, XrVector3f& direction);
void gaze_hold_report();
void gaze_thread_report(double seconds);

// quad_state.cpp
SessionGraphics session_graphics_now();
HelmetPieceSet& helmet_set();
bool views4_outline_on();
void views4_outline_draw(XrSwapchain swapchain);
void quad_fail(const char* why);
bool quad_ready();
void turn_view(XrView& view, double fractionX, double fractionY, double tanX, double tanY);
void focus_centre(const EyeComposition& eye, float& tanX, float& tanY);
bool sampled(uint64_t count);

// reports.cpp
int64_t qpc_now();
XrResult XRAPI_CALL timedEndFrameBelow(XrSession session, const XrFrameEndInfo* info);
XrResult XRAPI_CALL timedLocateViewsBelow(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
                                          uint32_t capacity, uint32_t* countOutput, XrView* views);
double report_pacing();
void report_load(double frameMs);
void report_cpu_times();
bool is_game_process();

// session.cpp
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

// helmet_layer.cpp
void helmet_layer_release();

// views4.cpp
XrExtent2Di views4_output_extent();

// helmet_layer.cpp
void log_helmet_light();
V4Helmet views4_helmet_start(const HelmetSettings& helmet);
void views4_helmet_eye(const HelmetSettings& helmet, V4Helmet& h, int e, EyeComposition& c, XrFovf& submitFov,
                       XrExtent2Di& extent);
void views4_helmet_layer(const HelmetSettings& helmet, const V4Helmet& h, const EyeComposition (&eyes)[2],
                         ID3D11Texture2D* views11);
void views4_helmet_submit(XrSession session, int layer, XrCompositionLayerQuad (&quads)[2], int& count, int& after);

// views4.cpp
XrResult XRAPI_CALL layerReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info);

// two_loops.cpp
void quad_record_a(const XrFrameEndInfo* info);
bool quad_capture_due();
void views4_capture(ID3D12Resource* views, ID3D12Resource* output);
XrResult quad_submit_b(XrSession session, const XrFrameEndInfo* info);
XrResult XRAPI_CALL layerGetVisibilityMaskKHR(XrSession session, XrViewConfigurationType type, uint32_t viewIndex,
                                              XrVisibilityMaskTypeKHR maskType, XrVisibilityMaskKHR* mask);

// views.cpp
void log_submitted_fov(uint32_t eye, const XrFovf& submitted, const XrFovf& composed);

// views4.cpp
XrResult views4_submit(XrSession session, const XrFrameEndInfo* info);

// views.cpp
XrResult XRAPI_CALL layerLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
                                     uint32_t capacity, uint32_t* countOutput, XrView* views);

// live_ini.cpp
void cheeky_apply_ini();
uint64_t ini_write_later(std::function<void()> write, IniWrite kind = IniWrite::Once);
void ini_worker_sync();
void live_reload();

// hotkey_watch.cpp
void pad_watch_ensure();
void pad_watch_stop();
void hotkeys_changed(const Hotkeys& h);
bool key_held(const KeyCombo& key);
bool pad_held(HotkeyAction action);
bool hotkey_held(HotkeyAction action);
std::wstring menu_hint();
std::wstring hotkey_footer(bool helmetOn);

// menu.cpp
int64_t qpc_ms(int64_t ms);
bool game_focused();
void menu_values_changed(const IniSnapshot& s);
void menu_placement_changed(const MenuPlacement& placement);
std::wstring game_settings_ini(const std::wstring& settings);
void menu_open(int64_t now);
void menu_press(MenuKey key, int64_t now);
const XrFrameEndInfo* menu_frame(XrSession session, const XrFrameEndInfo* info, XrFrameEndInfo& storage,
                                 std::vector<const XrCompositionLayerBaseHeader*>& layers);
void menu_shutdown();

// overlay.cpp
void notice_set(std::wstring title, std::vector<std::wstring> lines);
bool notice_pending();
void osd_waited(XrDuration period);
bool frame_generation_on();
bool osd_shown(OsdSummary& out);
void osd_wait_returned(double waitMs);
void osd_measure();
void osd_placement_changed();
void osd_placement_changed(const OsdPlacement& placement, const CheekySettings& cheeky);
const XrCompositionLayerBaseHeader* osd_frame(XrSession session, int64_t now);
void osd_shutdown();

// optimizer.cpp
void optimize_tick(int64_t now);
void frame_capture_poll();
void cpu_profile_poll();

// xr_layer.cpp
XrResult XRAPI_CALL layerEndFrame(XrSession session, const XrFrameEndInfo* info);
XrResult end_frame(XrSession session, const XrFrameEndInfo* frameInfo);
XrResult XRAPI_CALL layerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
XrResult XRAPI_CALL layerCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                const XrApiLayerCreateInfo* layerInfo, XrInstance* instance);

}  // namespace simqvfr::layer
