cmake_minimum_required(VERSION 3.24)
project(sim_qvfr LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "sim_qvfr is x64-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# The build uses Ninja (build.ps1, CONTRIBUTING.md), which runs about one job per logical processor across the
# whole build. A Visual Studio generator still works, and gets the same cap here (MSBuild's multi-tool task, enforced
# across projects); /MP per project, which this replaced, started up to one compiler per file per project at once and
# ran the machine out of memory.
set(CMAKE_VS_GLOBALS "UseMultiToolTask=true" "EnforceProcessCountAcrossBuilds=true")
# The standard and Windows headers most sources include, precompiled once per big target (sim_qvfr_core, sim_qvfr_layer,
# the launcher's two): a clean build takes about a fifth less. A source that sets macros before its own includes skips it.
set(SIM_QVFR_PCH <algorithm> <atomic> <cmath> <cstdint> <cstdio> <cstring> <filesystem> <map> <mutex> <string> <vector>
  <windows.h>)

# The release this code is headed for: the About pane names it, and package.ps1 uses it when -Version is left out.
set(SIM_QVFR_RELEASE "1.0.5-beta")
set(SIM_QVFR_VERSION "dev" CACHE STRING "Version shown by the launcher (package.ps1 sets it)")

# Windows version info for a binary (src/version_info.rc.in): numbers from the version's leading digits
# ("1.0.0-beta (abc1234)" -> 1,0,0,0), or from SIM_QVFR_RELEASE in a "dev" build.
set(SIM_QVFR_VERSION_TEXT "${SIM_QVFR_VERSION}")
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" _numbers "${SIM_QVFR_VERSION}")
if(NOT _numbers)
  string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" _numbers "${SIM_QVFR_RELEASE}")
  set(SIM_QVFR_VERSION_TEXT "${SIM_QVFR_VERSION} (${SIM_QVFR_RELEASE})")
endif()
string(REPLACE "." ";" _numbers "${_numbers}")
list(LENGTH _numbers _count)
while(_count LESS 4)
  list(APPEND _numbers 0)
  math(EXPR _count "${_count} + 1")
endwhile()
list(SUBLIST _numbers 0 4 _numbers)
list(JOIN _numbers "," SIM_QVFR_VERSION_COMMAS)
if(SIM_QVFR_VERSION_TEXT MATCHES "alpha|beta|rc|dev")
  set(SIM_QVFR_VERSION_FLAGS "VS_FF_PRERELEASE")
else()
  set(SIM_QVFR_VERSION_FLAGS "0")
endif()
function(sim_qvfr_version_info target file_name file_type description)
  set(SIM_QVFR_FILE_NAME "${file_name}")
  set(SIM_QVFR_FILE_TYPE "${file_type}")
  set(SIM_QVFR_FILE_DESCRIPTION "${description}")
  set(rc ${CMAKE_CURRENT_BINARY_DIR}/generated/${target}_version.rc)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/version_info.rc.in ${rc} @ONLY)
  target_sources(${target} PRIVATE ${rc})
endfunction()

# MinHook 1.3.4 and the OpenXR SDK headers, vendored (third_party/README.md).
set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_library(minhook STATIC
  ${THIRD_PARTY}/minhook/src/buffer.c
  ${THIRD_PARTY}/minhook/src/hook.c
  ${THIRD_PARTY}/minhook/src/trampoline.c
  ${THIRD_PARTY}/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${THIRD_PARTY}/minhook/include)

# Every source the layer shares with its tests except the layer itself (sim_qvfr_layer below), compiled once.
# A test links this and takes only the objects it references.
add_library(sim_qvfr_core STATIC
  src/games/games.cpp src/games/multi_view.cpp src/log.cpp src/layer_chain.cpp
  src/probe/probe.cpp src/probe/hang_watch.cpp
  src/probe/d3d11_capture.cpp src/probe/d3d12_trace.cpp src/probe/dred.cpp src/probe/pass_census.cpp src/probe/pix_capture.cpp src/probe/gpu_frame_timer.cpp src/probe/gpu_load.cpp src/probe/cpu_load.cpp src/probe/cpu_profile.cpp src/probe/vram_census.cpp src/probe/image_dump.cpp src/probe/run_exit.cpp
  src/probe/d3d12_vtable_index.c
  src/games/ace/double_render.cpp src/games/ace/views3.cpp src/games/ace/views4.cpp src/games/ace/batch_watch.cpp src/games/ace/dlss_extent.cpp src/games/ace/adapter.cpp src/games/acc/adapter.cpp src/games/acc/views.cpp src/games/acc/view_share.cpp src/games/acc/mesh_passes.cpp src/games/acc/gates.cpp src/games/acc/fixation.cpp src/games/xplane/adapter.cpp src/games/xplane/four_views.cpp src/games/xplane/four_views_shaders.cpp src/games/xplane/four_views_frame.cpp src/games/xplane/focus_projection.cpp src/games/xplane/spirv_patch.cpp src/games/lmu/adapter.cpp src/games/lmu/view_probe.cpp src/games/lmu/four_passes.cpp src/games/lmu/glare_share.cpp src/games/lmu/mvr_probe.cpp src/games/lmu/mvr_pass.cpp src/games/lmu/focus_post.cpp src/games/adapters.cpp src/engine/code_patch.cpp src/engine/object_vtable.cpp src/engine/d3d11_vtables.cpp src/engine/dxbc_patch.cpp src/engine/nvapi_stereo.cpp src/engine/nvapi_gpus.cpp
  src/quad/compositor.cpp src/quad/helmet_pieces.cpp src/quad/d3d11_compositor.cpp src/quad/d3d11_view_resample.cpp src/quad/d3d11_depth_probe.cpp src/quad/focus_outline.cpp src/quad/settings_menu.cpp src/quad/menu_panel.cpp
  src/quad/osd.cpp src/quad/osd_panel.cpp src/quad/qvfr_optimizer.cpp src/cheeky/cheeky_host.cpp
  src/input/hotkeys.cpp src/profile/profiles.cpp src/input/pad_input.cpp)
target_include_directories(sim_qvfr_core PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(sim_qvfr_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(sim_qvfr_core PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(sim_qvfr_core PUBLIC minhook bcrypt d3d12 dxgi d3dcompiler windowscodecs ole32 pdh dinput8 dxguid)
target_precompile_headers(sim_qvfr_core PRIVATE ${SIM_QVFR_PCH})
# C, and C-style COM declarations (CINTERFACE before its includes).
set_source_files_properties(src/probe/d3d12_vtable_index.c src/engine/d3d11_vtables.cpp
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# The layer (src/xr_layer.cpp and src/layer/), for the DLL and for two_loops_test and quad_flow_test. An OBJECT
# library, not a static one: every object is linked in, including xr_layer.cpp's exported
# xrNegotiateLoaderApiLayerInterface, which nothing in the DLL references.
add_library(sim_qvfr_layer OBJECT src/xr_layer.cpp src/layer/globals.cpp src/layer/config.cpp
  src/layer/gaze_actions.cpp src/layer/gaze.cpp src/layer/quad_state.cpp src/layer/reports.cpp src/layer/session.cpp
  src/layer/helmet_layer.cpp src/layer/views4.cpp src/layer/two_loops.cpp src/layer/views.cpp src/layer/live_ini.cpp
  src/layer/hotkey_watch.cpp src/layer/menu.cpp src/layer/overlay.cpp src/layer/optimizer.cpp src/layer/frame.cpp src/layer/native_quad.cpp)
target_include_directories(sim_qvfr_layer PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(sim_qvfr_layer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(sim_qvfr_layer PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(sim_qvfr_layer PUBLIC sim_qvfr_core)
target_precompile_headers(sim_qvfr_layer PRIVATE ${SIM_QVFR_PCH})

add_library(sim_qvfr SHARED src/early_load.cpp src/vk_early_layer.cpp)
target_include_directories(sim_qvfr PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(sim_qvfr PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(sim_qvfr PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(sim_qvfr PRIVATE sim_qvfr_layer)
set_target_properties(sim_qvfr PROPERTIES OUTPUT_NAME sim-qvfr)
sim_qvfr_version_info(sim_qvfr sim-qvfr.dll VFT_DLL "Sim-QVFR OpenXR layer")
# A linker map next to the DLL, to name the layer's frames in a game's crash call stack (no effect on the code).
target_link_options(sim_qvfr PRIVATE /MAP)

add_custom_command(TARGET sim_qvfr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/manifest/XR_APILAYER_SIM_qvfr.json
          $<TARGET_FILE_DIR:sim_qvfr>/XR_APILAYER_SIM_qvfr.json
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/manifest/VkLayer_SIM_qvfr_early.json
          $<TARGET_FILE_DIR:sim_qvfr>/vulkan/VkLayer_SIM_qvfr_early.json)

# Offline check of the layer chain (tests/layer_chain_test.cpp): it negotiates the built sim-qvfr.dll.
add_executable(layer_chain_test tests/layer_chain_test.cpp)
target_include_directories(layer_chain_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(layer_chain_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(layer_chain_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(layer_chain_test PRIVATE sim_qvfr_core)
add_dependencies(layer_chain_test sim_qvfr)

# Offline check of the hand-assembled probe stub (tests/stub_test.cpp).
add_executable(stub_test tests/stub_test.cpp)
target_include_directories(stub_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(stub_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(stub_test PRIVATE sim_qvfr_core)

# Offline check of the D3D12 swapchain-write trace on a WARP device (tests/d3d12_trace_test.cpp).
add_executable(d3d12_trace_test tests/d3d12_trace_test.cpp)
target_include_directories(d3d12_trace_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(d3d12_trace_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(d3d12_trace_test PRIVATE sim_qvfr_core)

# Offline check of the quad compositor on a WARP device (tests/compositor_test.cpp).
add_executable(compositor_test tests/compositor_test.cpp)
target_include_directories(compositor_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(compositor_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(compositor_test PRIVATE sim_qvfr_core)

# Offline check of the SSR resample on a WARP device, and ACC's viewport matching (tests/view_resample_test.cpp).
add_executable(view_resample_test tests/view_resample_test.cpp)
target_include_directories(view_resample_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(view_resample_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(view_resample_test PRIVATE sim_qvfr_core d3d11)

# Offline check of the depth probe on a WARP device (tests/depth_probe_test.cpp).
add_executable(depth_probe_test tests/depth_probe_test.cpp)
target_include_directories(depth_probe_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(depth_probe_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(depth_probe_test PRIVATE sim_qvfr_core d3d11)

# Offline check of the second-view vertex shader edit on a WARP device (tests/dxbc_patch_test.cpp).
add_executable(dxbc_patch_test tests/dxbc_patch_test.cpp)
target_include_directories(dxbc_patch_test PRIVATE src)
target_compile_definitions(dxbc_patch_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(dxbc_patch_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(dxbc_patch_test PRIVATE sim_qvfr_core d3d11 d3dcompiler)

# Hardware check of NVIDIA single-pass stereo with converted shaders (tests/nvapi_stereo_check.cpp): not in the suite
# (no _test suffix; needs an NVIDIA GPU). Optional argument: a folder of a game's vertex shaders, kept local.
add_executable(nvapi_stereo_check tests/nvapi_stereo_check.cpp)
target_include_directories(nvapi_stereo_check PRIVATE src)
target_compile_definitions(nvapi_stereo_check PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(nvapi_stereo_check PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(nvapi_stereo_check PRIVATE sim_qvfr_core d3d11 d3dcompiler)

# Offline check of LMU's MVR probe on a WARP device (tests/lmu_mvr_probe_test.cpp).
add_executable(lmu_mvr_probe_test tests/lmu_mvr_probe_test.cpp)
target_include_directories(lmu_mvr_probe_test PRIVATE src)
target_compile_definitions(lmu_mvr_probe_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(lmu_mvr_probe_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(lmu_mvr_probe_test PRIVATE sim_qvfr_core d3d11 d3dcompiler)

# Offline check of an object pointed at a vtable copy, on a WARP device (tests/object_vtable_test.cpp).
add_executable(object_vtable_test tests/object_vtable_test.cpp)
target_include_directories(object_vtable_test PRIVATE src)
target_compile_definitions(object_vtable_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(object_vtable_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(object_vtable_test PRIVATE sim_qvfr_core d3d11 d3dcompiler)

# Offline check of LMU's multi-view rendering in the eye passes on a WARP device (tests/lmu_mvr_pass_test.cpp).
add_executable(lmu_mvr_pass_test tests/lmu_mvr_pass_test.cpp)
target_include_directories(lmu_mvr_pass_test PRIVATE src)
target_compile_definitions(lmu_mvr_pass_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(lmu_mvr_pass_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(lmu_mvr_pass_test PRIVATE sim_qvfr_core d3d11 d3dcompiler)

# Offline check of LMU MVR's focus post (YEBIS's copy and composite replayed for the focus view) on a WARP device
# (tests/lmu_focus_post_test.cpp).
add_executable(lmu_focus_post_test tests/lmu_focus_post_test.cpp)
target_include_directories(lmu_focus_post_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(lmu_focus_post_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(lmu_focus_post_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(lmu_focus_post_test PRIVATE sim_qvfr_core d3d11 d3dcompiler)

# Offline check of the D3D11 compositor on a WARP device (tests/d3d11_compositor_test.cpp).
add_executable(d3d11_compositor_test tests/d3d11_compositor_test.cpp)
target_include_directories(d3d11_compositor_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(d3d11_compositor_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(d3d11_compositor_test PRIVATE sim_qvfr_core d3d11)

# Offline check of the compositor's sharpening (CAS) on a WARP device (tests/cas_sharpen_test.cpp).
add_executable(cas_sharpen_test tests/cas_sharpen_test.cpp)
target_include_directories(cas_sharpen_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(cas_sharpen_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(cas_sharpen_test PRIVATE sim_qvfr_core d3d11)

# Offline check of the panels' D3D11 upload on a WARP device (tests/panel_upload_test.cpp).
add_executable(panel_upload_test tests/panel_upload_test.cpp)
target_include_directories(panel_upload_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(panel_upload_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(panel_upload_test PRIVATE sim_qvfr_core d3d11)

# Offline check of the frame records carried to the composition (tests/frame_records_test.cpp).
add_executable(frame_records_test tests/frame_records_test.cpp)
target_include_directories(frame_records_test PRIVATE src)
target_compile_options(frame_records_test PRIVATE /W4 /permissive- /utf-8)

# Offline check of the two_loops frame pairing against a fake runtime (tests/two_loops_test.cpp).
add_executable(two_loops_test tests/two_loops_test.cpp)
target_include_directories(two_loops_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(two_loops_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(two_loops_test PRIVATE sim_qvfr_layer)

# Offline check of the quad pipeline's frame flow on a WARP device against a fake runtime (tests/quad_flow_test.cpp).
add_executable(quad_flow_test tests/quad_flow_test.cpp)
target_include_directories(quad_flow_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(quad_flow_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(quad_flow_test PRIVATE sim_qvfr_layer)

# Offline check of native quad views (quad/native_quad.h, layer/native_quad.cpp) against a fake runtime offering
# PRIMARY_QUAD_VARJO (tests/native_quad_test.cpp).
add_executable(native_quad_test tests/native_quad_test.cpp)
target_include_directories(native_quad_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(native_quad_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(native_quad_test PRIVATE sim_qvfr_layer)

# The decrypted-image dump (probe/image_dump.h): this exe's own code from memory, relocations undone, equals its file.
add_executable(image_dump_test tests/image_dump_test.cpp)
target_include_directories(image_dump_test PRIVATE src)
target_compile_definitions(image_dump_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(image_dump_test PRIVATE sim_qvfr_core)
target_link_options(image_dump_test PRIVATE /IGNORE:4078)  # its pointer placed in .text on purpose

# Gate runs that end themselves (probe/run_exit.h): WM_CLOSE to this process's visible top-level windows only.
add_executable(run_exit_test tests/run_exit_test.cpp)
target_include_directories(run_exit_test PRIVATE src)
target_compile_definitions(run_exit_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(run_exit_test PRIVATE sim_qvfr_core)

# LMU's focus projection (games/lmu/focus_math.h) against the projection the game builds.
add_executable(lmu_focus_test tests/lmu_focus_test.cpp)
target_include_directories(lmu_focus_test PRIVATE src)

# LMU's glare share (games/lmu/glare_math.h) on a WARP device: YEBIS's region, the eye's halo in the focus view at the
# same angles, the pointer walk to the texture (tests/lmu_glare_test.cpp).
add_executable(lmu_glare_test tests/lmu_glare_test.cpp)
target_include_directories(lmu_glare_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(lmu_glare_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(lmu_glare_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(lmu_glare_test PRIVATE sim_qvfr_core d3d11)

# ACE's DLSS output-extent fix (games/ace/ngx_extent.h) against a parameter bag laid out as NGX's.
add_executable(ace_ngx_extent_test tests/ace_ngx_extent_test.cpp)
target_include_directories(ace_ngx_extent_test PRIVATE src)

# Offline check of the views4 index-folding trampolines (tests/code_patch_test.cpp).
add_executable(code_patch_test tests/code_patch_test.cpp)
target_include_directories(code_patch_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(code_patch_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(code_patch_test PRIVATE sim_qvfr_core)

# Offline check of the DRED device-removal report on WARP (tests/dred_test.cpp).
add_executable(dred_test tests/dred_test.cpp)
target_include_directories(dred_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(dred_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dred_test PRIVATE sim_qvfr_core)

# Offline check of the views4 focus outline on WARP (tests/focus_outline_test.cpp).
add_executable(focus_outline_test tests/focus_outline_test.cpp)
target_include_directories(focus_outline_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(focus_outline_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(focus_outline_test PRIVATE sim_qvfr_core)

# Offline check of the per-pipeline pass census on WARP (tests/pass_census_test.cpp).
add_executable(pass_census_test tests/pass_census_test.cpp)
target_include_directories(pass_census_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(pass_census_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pass_census_test PRIVATE sim_qvfr_core)

# Offline check of the gaze turn on ACE's row-vector view matrices (tests/focus_turn_test.cpp).
add_executable(focus_turn_test tests/focus_turn_test.cpp)
target_include_directories(focus_turn_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(focus_turn_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# Offline check of the converging focus boxes (tests/focus_converge_test.cpp).
add_executable(focus_converge_test tests/focus_converge_test.cpp)
target_include_directories(focus_converge_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(focus_converge_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# Offline check of the in-headset settings menu and its panel image (tests/settings_menu_test.cpp).
add_executable(settings_menu_test tests/settings_menu_test.cpp)
target_include_directories(settings_menu_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(settings_menu_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# The documented ini, for timing a menu step on a file of its size (each ini write rewrites it).
target_compile_definitions(settings_menu_test PRIVATE SIM_QVFR_DOCUMENTED_INI="${CMAKE_CURRENT_SOURCE_DIR}/manifest/sim-qvfr.ini")
target_compile_options(settings_menu_test PRIVATE /utf-8)
target_link_libraries(settings_menu_test PRIVATE sim_qvfr_core)

# Offline check of the performance overlay: its numbers, rows and card (tests/osd_test.cpp).
add_executable(osd_test tests/osd_test.cpp)
target_include_directories(osd_test PRIVATE src)
target_compile_definitions(osd_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(osd_test PRIVATE /utf-8)
target_link_libraries(osd_test PRIVATE sim_qvfr_core)

# Offline check of the settings menu's QVFR optimizer against a simulated GPU (tests/qvfr_optimizer_test.cpp).
add_executable(qvfr_optimizer_test tests/qvfr_optimizer_test.cpp)
target_include_directories(qvfr_optimizer_test PRIVATE src)
target_compile_definitions(qvfr_optimizer_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(qvfr_optimizer_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(qvfr_optimizer_test PRIVATE sim_qvfr_core)

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
target_link_libraries(load_test PRIVATE sim_qvfr_core)
target_include_directories(gpu_frame_timer_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(gpu_frame_timer_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(gpu_frame_timer_test PRIVATE sim_qvfr_core)

# Offline check of the Cheeky runtime host against a fake runtime DLL (tests/cheeky_host_test.cpp); with
# SIM_QVFR_CHEEKY_RUNTIME set to a real CheekyFoveatedDLSSRuntime.dll it also starts that one.
add_library(cheeky_fake_runtime SHARED tests/cheeky_fake_runtime.cpp)
add_executable(cheeky_host_test tests/cheeky_host_test.cpp)
target_include_directories(cheeky_host_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(cheeky_host_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(cheeky_host_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(cheeky_host_test PRIVATE sim_qvfr_core)
add_dependencies(cheeky_host_test cheeky_fake_runtime)

# Offline check of the hang watch's module log and its minidump, written by the launcher as the dump helper
# (tests/hang_watch_test.cpp).
add_executable(hang_watch_test tests/hang_watch_test.cpp)
target_include_directories(hang_watch_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(hang_watch_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(hang_watch_test PRIVATE sim_qvfr_core)
add_dependencies(hang_watch_test sim_qvfr_launcher)  # the dump helper, next to the test

# Launcher: starts the chosen game in VR with this layer for one launch, above every implicit layer it can move
# (src/launcher/launch_plan.h). No OpenXR, MinHook or D3D dependency.
configure_file(src/launcher/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher_version.h @ONLY)
# The launcher's code that launcher_test checks, compiled once for both. An OBJECT library, so the launcher links every
# object as it did when they were its own sources (none has a static initializer, so their order doesn't matter).
add_library(sim_qvfr_launcher_shared OBJECT src/games/games.cpp src/launcher/settings.cpp src/launcher/launch_watch.cpp
  src/launcher/game_tabs.cpp src/launcher/report.cpp src/launcher/launch_plan.cpp src/launcher/layer_order.cpp
  src/launcher/explicit_layers.cpp src/launcher/session_history.cpp src/launcher/preflight.cpp src/input/hotkeys.cpp
  src/profile/profiles.cpp src/launcher/update_check.cpp src/launcher/install.cpp src/quad/osd.cpp)
target_include_directories(sim_qvfr_launcher_shared PRIVATE src)
target_compile_definitions(sim_qvfr_launcher_shared PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(sim_qvfr_launcher_shared PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(sim_qvfr_launcher_shared PUBLIC bcrypt winhttp version)
target_precompile_headers(sim_qvfr_launcher_shared PRIVATE ${SIM_QVFR_PCH})

add_executable(sim_qvfr_launcher WIN32 src/launcher/main.cpp src/input/pad_input.cpp src/engine/nvapi_gpus.cpp
  src/launcher/ui/launcher_window.cpp src/launcher/ui/canvas.cpp src/probe/hang_dump.cpp src/launcher/launcher.rc
  src/launcher/ui/window_state.cpp src/launcher/ui/app_actions.cpp src/launcher/ui/update_install.cpp
  src/launcher/ui/controls.cpp src/launcher/ui/help.cpp src/launcher/ui/sheet.cpp src/launcher/ui/form.cpp
  src/launcher/ui/pane_checks.cpp src/launcher/ui/pane_profiles.cpp src/launcher/ui/pane_resolution.cpp
  src/launcher/ui/pane_foveation.cpp src/launcher/ui/pane_dlss.cpp src/launcher/ui/pane_menu.cpp
  src/launcher/ui/pane_hotkeys.cpp src/launcher/ui/pane_games.cpp src/launcher/ui/pane_layers.cpp
  src/launcher/ui/pane_diagnostics.cpp src/launcher/ui/pane_report.cpp src/launcher/ui/pane_about.cpp
  src/launcher/ui/chrome.cpp src/launcher/ui/pointer.cpp src/launcher/ui/input.cpp)
target_include_directories(sim_qvfr_launcher PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(sim_qvfr_launcher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(sim_qvfr_launcher PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(sim_qvfr_launcher PRIVATE sim_qvfr_launcher_shared shell32 d2d1 dwrite dwmapi comdlg32 bcrypt dinput8 dxguid winhttp version ole32)
target_precompile_headers(sim_qvfr_launcher PRIVATE ${SIM_QVFR_PCH})
set_target_properties(sim_qvfr_launcher PROPERTIES OUTPUT_NAME sim-qvfr-launcher)
sim_qvfr_version_info(sim_qvfr_launcher sim-qvfr-launcher.exe VFT_APP "Sim-QVFR launcher")
# A development build gets the default settings next to the launcher, once: an edited copy is kept.
add_custom_command(TARGET sim_qvfr_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/manifest/sim-qvfr.ini
          -DDESTINATION=$<TARGET_FILE_DIR:sim_qvfr_launcher>/sim-qvfr.ini
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_if_missing.cmake)
# The helmet's pieces beside the layer, as package.ps1 ships them (a build never overwrites a user's own pieces), so a
# development build (a fresh worktree's too) draws the helmet.
add_custom_command(TARGET sim_qvfr_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:sim_qvfr_launcher>/helmet)
foreach(piece top bottom visor)
  add_custom_command(TARGET sim_qvfr_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/assets/helmet/${piece}.png
            -DDESTINATION=$<TARGET_FILE_DIR:sim_qvfr_launcher>/helmet/${piece}.png
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_if_missing.cmake)
endforeach()

# Offline check of the launcher's launch plan (tests/launcher_test.cpp).
add_executable(launcher_test tests/launcher_test.cpp)
target_include_directories(launcher_test PRIVATE src)
target_compile_definitions(launcher_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(launcher_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(launcher_test PRIVATE sim_qvfr_launcher_shared)

# Offline check of the VRAM census on a WARP device: textures recorded, sorted and taken off when freed
# (tests/vram_census_test.cpp).
add_executable(vram_census_test tests/vram_census_test.cpp)
target_include_directories(vram_census_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(vram_census_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(vram_census_test PRIVATE sim_qvfr_core)

# Offline check of the CPU sampling profiler: a spinning thread's function on top (tests/cpu_profile_test.cpp).
add_executable(cpu_profile_test tests/cpu_profile_test.cpp)
target_include_directories(cpu_profile_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(cpu_profile_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(cpu_profile_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(cpu_profile_test PRIVATE sim_qvfr_core)

# Offline check of X-Plane's focus projections (tests/xplane_focus_test.cpp).
add_executable(xplane_focus_test tests/xplane_focus_test.cpp src/games/xplane/focus_projection.cpp)
target_include_directories(xplane_focus_test PRIVATE src)
target_compile_options(xplane_focus_test PRIVATE /W4 /permissive- /utf-8)

# Offline check of the SPIR-V edit for X-Plane's final resolve shader (tests/xplane_spirv_test.cpp).
add_executable(xplane_spirv_test tests/xplane_spirv_test.cpp src/games/xplane/spirv_patch.cpp)
target_include_directories(xplane_spirv_test PRIVATE src)
target_compile_definitions(xplane_spirv_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(xplane_spirv_test PRIVATE /W4 /permissive- /utf-8)

# Offline check of ACC's four-view mapping onto UE 4.26's stereo passes (tests/acc_views_test.cpp).
add_executable(acc_views_test tests/acc_views_test.cpp)
target_include_directories(acc_views_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(acc_views_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(acc_views_test PRIVATE /W4 /permissive- /utf-8)

# Offline check of ACC's gate comparisons (tests/acc_gates_test.cpp).
add_executable(acc_gates_test tests/acc_gates_test.cpp)
target_include_directories(acc_gates_test PRIVATE src)
target_compile_options(acc_gates_test PRIVATE /W4 /permissive- /utf-8)

# The offline suite for ctest: every *_test executable above, side by side
# (`ctest --test-dir build -C Release -j 16 --output-on-failure`, `-R lmu` for some). Each test gets its own TEMP folder
# and log, so tests that use the same file names never meet, in one run or in two worktrees' runs at once. The folders
# are under the user's TEMP, not the build folder: some tests compare file times, which an exFAT drive keeps coarser.
enable_testing()
string(MD5 _build_hash ${CMAKE_CURRENT_BINARY_DIR})
string(SUBSTRING ${_build_hash} 0 8 _build_hash)
file(TO_CMAKE_PATH "$ENV{TEMP}" _temp)
set(_scratch_root ${_temp}/sim-qvfr-tests/${_build_hash})
set(_scratch_folders)
get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _targets)
  get_target_property(_type ${_target} TYPE)
  if(_target MATCHES "_test$" AND _type STREQUAL "EXECUTABLE")
    # Deterministic links (no time stamp in the exe): a test the change didn't touch relinks to the same bytes, which
    # Windows Defender has already checked, instead of a new exe it checks for about a second on its first run.
    target_link_options(${_target} PRIVATE /Brepro)
    file(TO_NATIVE_PATH ${_scratch_root}/${_target} _scratch)
    list(APPEND _scratch_folders ${_scratch})
    add_test(NAME ${_target} COMMAND ${_target})
    set_tests_properties(${_target} PROPERTIES
      ENVIRONMENT "TEMP=${_scratch};TMP=${_scratch};SIM_QVFR_LOG=${_scratch}\\test.log"
      FIXTURES_REQUIRED test_scratch
      TIMEOUT 120)
  endif()
endforeach()
add_test(NAME test_scratch COMMAND ${CMAKE_COMMAND} -E make_directory ${_scratch_folders})
set_tests_properties(test_scratch PROPERTIES FIXTURES_SETUP test_scratch)
