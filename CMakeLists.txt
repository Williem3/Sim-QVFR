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

add_library(ace_quadview SHARED
  src/xr_layer.cpp
  src/log.cpp
  src/probe/probe.cpp
  src/probe/d3d12_trace.cpp src/probe/dred.cpp src/probe/pass_census.cpp src/probe/pix_capture.cpp src/probe/gpu_frame_timer.cpp
  src/probe/d3d12_vtable_index.c
  src/experiment/double_render.cpp src/experiment/views3.cpp src/experiment/views4.cpp src/engine/code_patch.cpp
  src/quad/compositor.cpp src/quad/focus_outline.cpp)
target_include_directories(ace_quadview PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(ace_quadview PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ace_quadview PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(ace_quadview PRIVATE minhook bcrypt d3d12 d3dcompiler windowscodecs ole32)
set_target_properties(ace_quadview PROPERTIES OUTPUT_NAME ace-quadview)

add_custom_command(TARGET ace_quadview POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/manifest/XR_APILAYER_ACE_quadview.json
          $<TARGET_FILE_DIR:ace_quadview>/XR_APILAYER_ACE_quadview.json)

# Offline check of the hand-assembled probe stub (tests/stub_test.cpp).
add_executable(stub_test tests/stub_test.cpp src/log.cpp)
target_include_directories(stub_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(stub_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(stub_test PRIVATE minhook bcrypt)

# Offline check of the D3D12 swapchain-write trace on a WARP device (tests/d3d12_trace_test.cpp).
add_executable(d3d12_trace_test tests/d3d12_trace_test.cpp src/probe/d3d12_trace.cpp src/probe/dred.cpp src/probe/d3d12_vtable_index.c src/log.cpp)
target_include_directories(d3d12_trace_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(d3d12_trace_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(d3d12_trace_test PRIVATE d3d12 dxgi)

# Offline check of the quad compositor on a WARP device (tests/compositor_test.cpp).
add_executable(compositor_test tests/compositor_test.cpp src/quad/compositor.cpp src/quad/focus_outline.cpp src/log.cpp)
target_include_directories(compositor_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(compositor_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(compositor_test PRIVATE d3d12 dxgi d3dcompiler windowscodecs ole32)

# Offline check of the two_loops frame pairing against a fake runtime (tests/two_loops_test.cpp).
add_executable(two_loops_test tests/two_loops_test.cpp src/log.cpp src/probe/probe.cpp src/probe/d3d12_trace.cpp src/probe/dred.cpp src/probe/pass_census.cpp src/probe/pix_capture.cpp src/probe/gpu_frame_timer.cpp
  src/probe/d3d12_vtable_index.c src/experiment/double_render.cpp src/experiment/views3.cpp src/experiment/views4.cpp src/engine/code_patch.cpp src/quad/compositor.cpp src/quad/focus_outline.cpp)
target_include_directories(two_loops_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(two_loops_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(two_loops_test PRIVATE minhook bcrypt d3d12 d3dcompiler windowscodecs ole32)

# Offline check of the quad pipeline's frame flow on a WARP device against a fake runtime (tests/quad_flow_test.cpp).
add_executable(quad_flow_test tests/quad_flow_test.cpp src/log.cpp src/probe/probe.cpp src/probe/d3d12_trace.cpp src/probe/dred.cpp src/probe/pass_census.cpp src/probe/pix_capture.cpp src/probe/gpu_frame_timer.cpp
  src/probe/d3d12_vtable_index.c src/experiment/double_render.cpp src/experiment/views3.cpp src/experiment/views4.cpp src/engine/code_patch.cpp src/quad/compositor.cpp src/quad/focus_outline.cpp)
target_include_directories(quad_flow_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(quad_flow_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(quad_flow_test PRIVATE minhook bcrypt d3d12 dxgi d3dcompiler windowscodecs ole32)

# Offline check of the views4 index-folding trampolines (tests/code_patch_test.cpp).
add_executable(code_patch_test tests/code_patch_test.cpp src/engine/code_patch.cpp src/log.cpp)
target_include_directories(code_patch_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(code_patch_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# Offline check of the DRED device-removal report on WARP (tests/dred_test.cpp).
add_executable(dred_test tests/dred_test.cpp src/probe/dred.cpp src/log.cpp)
target_include_directories(dred_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(dred_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dred_test PRIVATE d3d12 dxgi)

# Offline check of the views4 focus outline on WARP (tests/focus_outline_test.cpp).
add_executable(focus_outline_test tests/focus_outline_test.cpp src/quad/focus_outline.cpp src/log.cpp)
target_include_directories(focus_outline_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(focus_outline_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(focus_outline_test PRIVATE d3d12 dxgi)

# Offline check of the per-pipeline pass census on WARP (tests/pass_census_test.cpp).
add_executable(pass_census_test tests/pass_census_test.cpp src/probe/pass_census.cpp src/probe/d3d12_vtable_index.c src/log.cpp)
target_include_directories(pass_census_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(pass_census_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pass_census_test PRIVATE d3d12 dxgi d3dcompiler dxguid)

# Offline check of the gaze turn on ACE's row-vector view matrices (tests/focus_turn_test.cpp).
add_executable(focus_turn_test tests/focus_turn_test.cpp)
target_include_directories(focus_turn_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(focus_turn_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# Offline check of the focus-view cull planes and tests (tests/focus_cull_test.cpp).
add_executable(focus_cull_test tests/focus_cull_test.cpp)
target_include_directories(focus_cull_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(focus_cull_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# Offline check of the GPU frame timer on a WARP device (tests/gpu_frame_timer_test.cpp).
add_executable(gpu_frame_timer_test tests/gpu_frame_timer_test.cpp src/probe/gpu_frame_timer.cpp src/log.cpp)
target_include_directories(gpu_frame_timer_test PRIVATE src ${THIRD_PARTY}/openxr/include)
target_compile_definitions(gpu_frame_timer_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(gpu_frame_timer_test PRIVATE d3d12 dxgi)

# Launcher: starts ACE in VR with this layer for one launch, above every implicit layer it can move
# (src/launcher/launch_plan.h). No OpenXR, MinHook or D3D dependency.
set(ACE_QV_VERSION "dev" CACHE STRING "Version shown by the launcher (package.ps1 sets it)")
configure_file(src/launcher/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher_version.h @ONLY)
add_executable(ace_launcher src/launcher/main.cpp src/launcher/launch_plan.cpp)
target_include_directories(ace_launcher PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(ace_launcher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ace_launcher PRIVATE /W4 /permissive- /utf-8)
set_target_properties(ace_launcher PROPERTIES OUTPUT_NAME ace-quadview-launcher)

# Offline check of the launcher's launch plan (tests/launcher_test.cpp).
add_executable(launcher_test tests/launcher_test.cpp src/launcher/launch_plan.cpp)
target_include_directories(launcher_test PRIVATE src)
target_compile_definitions(launcher_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(launcher_test PRIVATE /W4 /permissive- /utf-8)
