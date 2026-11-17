// NVIDIA Nsight Aftermath for GPU hangs (development builds with the SDK only: CMake's AFTERMATH_SDK_DIR, local and never
// committed, like NVIDIA's DLSS SDK). Off unless [experiment] aftermath=1. Without the SDK every call does nothing.
//   - GPU crash dumps: enabled before the game makes its device (enable_dumps); on a hang or fault the dump is written
//     to logs\gpu-crash-<n>.nv-gpudmp with its decoded JSON beside it, and the log names the device's state and the
//     markers the GPU reached last.
//   - Markers: numbers put into the device context's command stream (mark: no copy, no allocation); after a hang the
//     last one the GPU reached on each context is in the dump and from markers_after_removal. On D3D11 NVIDIA's driver
//     takes markers only while the "Nsight Aftermath GPU Crash Dump Monitor" (Nsight Graphics) runs on the PC.
// A marker's number is the caller's (set_marker_names turns one into words for the log).
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace simqvfr::aftermath {

// Built with the SDK (a development build).
bool built();
// Before the game makes its D3D11 device (xrCreateInstance): loads the library and enables GPU crash dumps.
// False: not built in, the library missing, or Aftermath refused (logged).
bool enable_dumps();
// The game's device and its immediate context (the context's thread): Aftermath initialised on the device with
// markers, and a context handle for them. False: off (logged).
bool attach(ID3D11Device* device, ID3D11DeviceContext* context);
// A marker into the attached context's command stream (its thread only). Nothing when off.
void mark(uint64_t code);
bool marking();
// For the log: a marker's number in words (`out`, `size` bytes).
void set_marker_names(void (*name)(uint64_t code, char* out, unsigned size));
// The device was found removed: the device's state and the last marker the GPU reached on the context, logged; then up
// to `waitMs` for the crash dump to be written.
void after_removal(unsigned waitMs);
// Session end (the context's thread): the context handle let go.
void detach();

}  // namespace simqvfr::aftermath
