cmake_minimum_required(VERSION 3.24)
project(ace_quadview LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ace_quadview is x64-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook 1.3.4 and the OpenXR SDK headers, vendored (third_party/README.md).
set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_library(minhook STATIC
  ${THIRD_PARTY}/minhook/src/buffer.c
  ${THIRD_PARTY}/minhook/src/hook.c
  ${THIRD_PARTY}/minhook/src/trampoline.c
  ${THIRD_PARTY}/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${THIRD_PARTY}/minhook/include)

# Every source the layer shares with its tests except src/xr_layer.cpp (two_loops_test and quad_flow_test
# #include it), compiled once. A test links this and takes only the objects it references.
add_library(ace_qv_core STATIC
  src/games/games.cpp src/log.cpp
  src/probe/probe.cpp src/probe/hang_watch.cpp
  src/probe/d3d11_capture.cpp src/probe/d3d12_trace.cpp src/probe/dred.cpp src/probe/pass_census.cpp src/probe/pix_capture.cpp src/probe/gpu_frame_timer.cpp src/probe/gpu_load.cpp src/probe/cpu_load.cpp src/probe/cpu_profile.cpp src/probe/vram_census.cpp
  src/probe/d3d12_vtable_index.c
  src/games/ace/double_render.cpp src/games/ace/views3.cpp src/games/ace/views4.cpp src/games/ace/batch_watch.cpp src/games/ace/adapter.cpp src/games/acc/adapter.cpp src/games/acc/views.cpp src/games/acc/view_share.cpp src/games/adapters.cpp src/engine/code_patch.cpp
  src/quad/compositor.cpp src/quad/d3d11_compositor.cpp src/quad/d3d11_view_resample.cpp src/quad/focus_outline.cpp src/quad/settings_menu.cpp src/quad/menu_panel.cpp
  src/quad/osd.cpp src/quad/osd_panel.cpp src/quad/qvfr_optimizer.cpp src/cheeky/cheeky_host.cpp
  src/input/hotkeys.cpp src/profile/profiles.cpp src/input/pad_input.cpp)
target_include_directories(ace_qv_core PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(ace_qv_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ace_qv_core PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(ace_qv_core PUBLIC minhook bcrypt d3d12 dxgi d3dcompiler windowscodecs ole32 pdh dinput8 dxguid)

add_library(ace_quadview SHARED src/xr_layer.cpp)
target_include_directories(ace_quadview PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(ace_quadview PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ace_quadview PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(ace_quadview PRIVATE ace_qv_core)
set_target_properties(ace_quadview PROPERTIES OUTPUT_NAME ace-quadview)
# A linker map next to the DLL, to name the layer's frames in a game's crash call stack (no effect on the code).
target_link_options(ace_quadview PRIVATE /MAP)

add_custom_command(TARGET ace_quadview POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/manifest/XR_APILAYER_ACE_quadview.json
          $<TARGET_FILE_DIR:ace_quadview>/XR_APILAYER_ACE_quadview.json)

# Offline check of the hand-assembled probe stub (tests/stub_test.cpp).
add_executable(stub_test tests/stub_test.cpp)
target_include_directories(stub_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(stub_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(stub_test PRIVATE ace_qv_core)

# Offline check of the D3D12 swapchain-write trace on a WARP device (tests/d3d12_trace_test.cpp).
add_executable(d3d12_trace_test tests/d3d12_trace_test.cpp)
target_include_directories(d3d12_trace_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(d3d12_trace_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(d3d12_trace_test PRIVATE ace_qv_core)

# Offline check of the quad compositor on a WARP device (tests/compositor_test.cpp).
add_executable(compositor_test tests/compositor_test.cpp)
target_include_directories(compositor_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(compositor_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(compositor_test PRIVATE ace_qv_core)

# Offline check of the SSR resample on a WARP device, and ACC's viewport matching (tests/view_resample_test.cpp).
add_executable(view_resample_test tests/view_resample_test.cpp)
target_include_directories(view_resample_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(view_resample_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(view_resample_test PRIVATE ace_qv_core d3d11)

# Offline check of the D3D11 compositor on a WARP device (tests/d3d11_compositor_test.cpp).
add_executable(d3d11_compositor_test tests/d3d11_compositor_test.cpp)
target_include_directories(d3d11_compositor_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(d3d11_compositor_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(d3d11_compositor_test PRIVATE ace_qv_core d3d11)

# Offline check of the panels' D3D11 upload on a WARP device (tests/panel_upload_test.cpp).
add_executable(panel_upload_test tests/panel_upload_test.cpp)
target_include_directories(panel_upload_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(panel_upload_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(panel_upload_test PRIVATE ace_qv_core d3d11)

# Offline check of the frame records carried to the composition (tests/frame_records_test.cpp).
add_executable(frame_records_test tests/frame_records_test.cpp)
target_include_directories(frame_records_test PRIVATE src)
target_compile_options(frame_records_test PRIVATE /W4 /permissive- /utf-8)

# Offline check of the two_loops frame pairing against a fake runtime (tests/two_loops_test.cpp).
add_executable(two_loops_test tests/two_loops_test.cpp)
target_include_directories(two_loops_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(two_loops_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(two_loops_test PRIVATE ace_qv_core)

# Offline check of the quad pipeline's frame flow on a WARP device against a fake runtime (tests/quad_flow_test.cpp).
add_executable(quad_flow_test tests/quad_flow_test.cpp)
target_include_directories(quad_flow_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(quad_flow_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(quad_flow_test PRIVATE ace_qv_core)

# Offline check of the views4 index-folding trampolines (tests/code_patch_test.cpp).
add_executable(code_patch_test tests/code_patch_test.cpp)
target_include_directories(code_patch_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(code_patch_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(code_patch_test PRIVATE ace_qv_core)

# Offline check of the DRED device-removal report on WARP (tests/dred_test.cpp).
add_executable(dred_test tests/dred_test.cpp)
target_include_directories(dred_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(dred_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dred_test PRIVATE ace_qv_core)

# Offline check of the views4 focus outline on WARP (tests/focus_outline_test.cpp).
add_executable(focus_outline_test tests/focus_outline_test.cpp)
target_include_directories(focus_outline_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(focus_outline_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(focus_outline_test PRIVATE ace_qv_core)

# Offline check of the per-pipeline pass census on WARP (tests/pass_census_test.cpp).
add_executable(pass_census_test tests/pass_census_test.cpp)
target_include_directories(pass_census_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(pass_census_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pass_census_test PRIVATE ace_qv_core)

# Offline check of the gaze turn on ACE's row-vector view matrices (tests/focus_turn_test.cpp).
add_executable(focus_turn_test tests/focus_turn_test.cpp)
target_include_directories(focus_turn_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(focus_turn_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# Offline check of the in-headset settings menu and its panel image (tests/settings_menu_test.cpp).
add_executable(settings_menu_test tests/settings_menu_test.cpp)
target_include_directories(settings_menu_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(settings_menu_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(settings_menu_test PRIVATE /utf-8)
target_link_libraries(settings_menu_test PRIVATE ace_qv_core)

# Offline check of the performance overlay: its numbers, rows and card (tests/osd_test.cpp).
add_executable(osd_test tests/osd_test.cpp)
target_include_directories(osd_test PRIVATE src)
target_compile_definitions(osd_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(osd_test PRIVATE /utf-8)
target_link_libraries(osd_test PRIVATE ace_qv_core)

# Offline check of the settings menu's QVFR optimizer against a simulated GPU (tests/qvfr_optimizer_test.cpp).
add_executable(qvfr_optimizer_test tests/qvfr_optimizer_test.cpp)
target_include_directories(qvfr_optimizer_test PRIVATE src)
target_compile_definitions(qvfr_optimizer_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(qvfr_optimizer_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(qvfr_optimizer_test PRIVATE ace_qv_core)

# Offline check of the focus-view cull planes and tests (tests/focus_cull_test.cpp).
add_executable(focus_cull_test tests/focus_cull_test.cpp)
target_include_directories(focus_cull_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(focus_cull_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# Offline check of the GPU frame timer on a WARP device (tests/gpu_frame_timer_test.cpp).
add_executable(gpu_frame_timer_test tests/gpu_frame_timer_test.cpp)

# Offline check of the GPU and CPU load samplers (tests/load_test.cpp).
add_executable(load_test tests/load_test.cpp)
target_include_directories(load_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(load_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(load_test PRIVATE ace_qv_core)
target_include_directories(gpu_frame_timer_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(gpu_frame_timer_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(gpu_frame_timer_test PRIVATE ace_qv_core)

# Offline check of the Cheeky runtime host against a fake runtime DLL (tests/cheeky_host_test.cpp); with
# ACE_QV_CHEEKY_RUNTIME set to a real CheekyFoveatedDLSSRuntime.dll it also starts that one.
add_library(cheeky_fake_runtime SHARED tests/cheeky_fake_runtime.cpp)
add_executable(cheeky_host_test tests/cheeky_host_test.cpp)
target_include_directories(cheeky_host_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(cheeky_host_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(cheeky_host_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(cheeky_host_test PRIVATE ace_qv_core)
add_dependencies(cheeky_host_test cheeky_fake_runtime)

# Offline check of the start-up hang watch's module log and minidump (tests/hang_watch_test.cpp).
add_executable(hang_watch_test tests/hang_watch_test.cpp)
target_include_directories(hang_watch_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(hang_watch_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(hang_watch_test PRIVATE ace_qv_core)

# Launcher: starts ACE in VR with this layer for one launch, above every implicit layer it can move
# (src/launcher/launch_plan.h). No OpenXR, MinHook or D3D dependency.
set(ACE_QV_VERSION "dev" CACHE STRING "Version shown by the launcher (package.ps1 sets it)")
configure_file(src/launcher/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher_version.h @ONLY)
add_executable(ace_launcher WIN32 src/launcher/main.cpp src/games/games.cpp src/launcher/settings.cpp src/launcher/report.cpp src/launcher/launch_plan.cpp src/launcher/layer_order.cpp
  src/launcher/preflight.cpp src/input/hotkeys.cpp src/profile/profiles.cpp src/input/pad_input.cpp src/launcher/update_check.cpp src/launcher/install.cpp
  src/quad/osd.cpp src/launcher/ui/launcher_window.cpp src/launcher/ui/canvas.cpp src/launcher/launcher.rc)
target_include_directories(ace_launcher PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(ace_launcher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ace_launcher PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(ace_launcher PRIVATE shell32 d2d1 dwrite dwmapi comdlg32 bcrypt dinput8 dxguid winhttp)
set_target_properties(ace_launcher PROPERTIES OUTPUT_NAME ace-quadview-launcher)

# Offline check of the launcher's launch plan (tests/launcher_test.cpp).
add_executable(launcher_test tests/launcher_test.cpp src/games/games.cpp src/launcher/launch_plan.cpp src/launcher/layer_order.cpp src/launcher/settings.cpp src/launcher/report.cpp src/quad/osd.cpp
  src/launcher/preflight.cpp src/input/hotkeys.cpp src/profile/profiles.cpp src/launcher/update_check.cpp src/launcher/install.cpp)
target_include_directories(launcher_test PRIVATE src)
target_compile_definitions(launcher_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(launcher_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(launcher_test PRIVATE bcrypt winhttp)

# Offline check of the VRAM census on a WARP device: textures recorded, sorted and taken off when freed
# (tests/vram_census_test.cpp).
add_executable(vram_census_test tests/vram_census_test.cpp)
target_include_directories(vram_census_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(vram_census_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(vram_census_test PRIVATE ace_qv_core)

# Offline check of the CPU sampling profiler: a spinning thread's function on top (tests/cpu_profile_test.cpp).
add_executable(cpu_profile_test tests/cpu_profile_test.cpp)
target_include_directories(cpu_profile_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(cpu_profile_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(cpu_profile_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(cpu_profile_test PRIVATE ace_qv_core)

# Offline check of ACC's four-view mapping onto UE 4.26's stereo passes (tests/acc_views_test.cpp).
add_executable(acc_views_test tests/acc_views_test.cpp)
target_include_directories(acc_views_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(acc_views_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(acc_views_test PRIVATE /W4 /permissive- /utf-8)
