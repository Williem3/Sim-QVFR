// D3D12 Device Removed Extended Data: when the GPU faults and the device is removed, logs which
// command lists had not finished, the last GPU operations and marker strings around the fault,
// and the resources at a page-fault address. CPU crash stacks cannot show this.
#pragma once

struct ID3D12Device;

namespace simqvfr {

// Must run before the app creates its D3D12 device (xrCreateInstance is early enough).
bool dred_enable();
// Polls the device for removal on a background thread and logs the DRED report once.
void dred_watch(ID3D12Device* device);
void dred_stop();
// Logs the report now if the device has been removed. Returns true if it was. Used by the test.
bool dred_report_if_removed(ID3D12Device* device);

// D3D12 debug layer: validates every API call. Enable before device creation; attach logs its
// errors and warnings (the first few of each message ID) through an info-queue callback.
// gpuValidation: also GPU-based validation (every shader access checked; very slow).
bool d3d12_debug_enable(bool gpuValidation = false);
void d3d12_debug_attach(ID3D12Device* device);

}  // namespace simqvfr
