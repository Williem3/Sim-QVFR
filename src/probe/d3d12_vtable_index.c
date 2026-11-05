/* Vtable slot numbers taken from the SDK's C interface definitions, so none are hand-counted. */
#define CINTERFACE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#pragma warning(disable : 4201) /* nameless unions in the SDK's C headers */
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>

#define SLOT(vtbl, method) (offsetof(vtbl, method) / sizeof(void*))

size_t simqvfr_slot_CreateRenderTargetView(void) { return SLOT(ID3D12DeviceVtbl, CreateRenderTargetView); }
size_t simqvfr_slot_OMSetRenderTargets(void) { return SLOT(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets); }
size_t simqvfr_slot_RSSetViewports(void) { return SLOT(ID3D12GraphicsCommandListVtbl, RSSetViewports); }
size_t simqvfr_slot_BeginRenderPass(void) { return SLOT(ID3D12GraphicsCommandList4Vtbl, BeginRenderPass); }
size_t simqvfr_slot_CreateCommittedResource(void) { return SLOT(ID3D12DeviceVtbl, CreateCommittedResource); }
size_t simqvfr_slot_CreatePlacedResource(void) { return SLOT(ID3D12DeviceVtbl, CreatePlacedResource); }
size_t simqvfr_slot_CreateCommittedResource1(void) { return SLOT(ID3D12Device4Vtbl, CreateCommittedResource1); }
size_t simqvfr_slot_CreateCommittedResource2(void) { return SLOT(ID3D12Device8Vtbl, CreateCommittedResource2); }
size_t simqvfr_slot_CreatePlacedResource1(void) { return SLOT(ID3D12Device8Vtbl, CreatePlacedResource1); }
size_t simqvfr_slot_CreateCommittedResource3(void) { return SLOT(ID3D12Device10Vtbl, CreateCommittedResource3); }
size_t simqvfr_slot_CreatePlacedResource2(void) { return SLOT(ID3D12Device10Vtbl, CreatePlacedResource2); }
size_t simqvfr_slot_SetPipelineState(void) { return SLOT(ID3D12GraphicsCommandListVtbl, SetPipelineState); }
size_t simqvfr_slot_DrawInstanced(void) { return SLOT(ID3D12GraphicsCommandListVtbl, DrawInstanced); }
size_t simqvfr_slot_DrawIndexedInstanced(void) { return SLOT(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced); }
size_t simqvfr_slot_Dispatch(void) { return SLOT(ID3D12GraphicsCommandListVtbl, Dispatch); }
size_t simqvfr_slot_ExecuteIndirect(void) { return SLOT(ID3D12GraphicsCommandListVtbl, ExecuteIndirect); }
