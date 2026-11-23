// The focus core (replay_internal.h has the file map): our compute pipelines, the twins of the game's targets and
// their view slots, retiring and pruning, the rebuild after a game remake, and the focus scene replay recorded at the
// Close of each replayable stereo list.
#include "replay_internal.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../log.h"
#include "../../quad/focus_mask.h"
#include "replay_shaders.h"

namespace simqvfr::replay_detail {

bool focus_compile() {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kPatchShader, sizeof(kPatchShader) - 1, "focus-constants", nullptr, nullptr, "main", "cs_5_0", 0,
                          0, &code, &errors))) {
        logf("replay focus: the constants shader did not compile: %s",
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {1, 0, kPatchParams};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 0};
    const D3D12_ROOT_SIGNATURE_DESC desc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&focus.patchRoot)))) {
        log_line("replay focus: root signature failed");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = focus.patchRoot.Get();
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&focus.patchPso)))) {
        log_line("replay focus: compute pipeline failed");
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kSlots} * kSlotBytes, 1, 1, 1,
                               DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&focus.constants))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&focus.fence)))) {
        log_line("replay focus: constants buffer or fence failed");
        return false;
    }
    for (int i = kRtvSlots - 1; i >= 0; --i) focus.freeRtv.push_back(i);
    for (int i = kDsvSlots - 1; i >= 0; --i) focus.freeDsv.push_back(i);
    {  // frameData with the focus tile grid
        ComPtr<ID3DBlob> frameCode, frameErrors;
        D3D12_COMPUTE_PIPELINE_STATE_DESC framePso{};
        framePso.pRootSignature = focus.patchRoot.Get();
        if (SUCCEEDED(D3DCompile(kFrameShader, sizeof(kFrameShader) - 1, "focus-frame-data", nullptr, nullptr, "main", "cs_5_0",
                                 0, 0, &frameCode, &frameErrors))) {
            framePso.CS = {frameCode->GetBufferPointer(), frameCode->GetBufferSize()};
            if (FAILED(device->CreateComputePipelineState(&framePso, IID_PPV_ARGS(&focus.framePso))))
                log_line("replay focus: tiles: the frameData copy could not be made; the focus views keep the game's tile grid");
        }
    }
    if (focus.settings.tyres) {  // the right eye's tyres: the constants copy
        ComPtr<ID3DBlob> tyreCode, tyreErrors, tyreBlob;
        D3D12_ROOT_PARAMETER tyreParams[2] = {};
        tyreParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        tyreParams[0].Descriptor = {0, 0};
        tyreParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        tyreParams[1].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC tyreDesc{2, tyreParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_RESOURCE_DESC tyreBuffer = buffer;
        tyreBuffer.Width = UINT64{kTyreSlots} * kTyreSlotBytes;
        bool okTyre = SUCCEEDED(D3DCompile(kTyreRightShader, sizeof(kTyreRightShader) - 1, "tyre-right", nullptr, nullptr,
                                           "main", "cs_5_0", 0, 0, &tyreCode, &tyreErrors)) &&
                      SUCCEEDED(D3D12SerializeRootSignature(&tyreDesc, D3D_ROOT_SIGNATURE_VERSION_1, &tyreBlob, &tyreErrors)) &&
                      SUCCEEDED(device->CreateRootSignature(0, tyreBlob->GetBufferPointer(), tyreBlob->GetBufferSize(),
                                                            IID_PPV_ARGS(&focus.tyreRoot)));
        if (okTyre) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC tyrePso{};
            tyrePso.pRootSignature = focus.tyreRoot.Get();
            tyrePso.CS = {tyreCode->GetBufferPointer(), tyreCode->GetBufferSize()};
            okTyre = SUCCEEDED(device->CreateComputePipelineState(&tyrePso, IID_PPV_ARGS(&focus.tyrePso))) &&
                     SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tyreBuffer,
                                                               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr,
                                                               IID_PPV_ARGS(&focus.tyreConstants)));
        }
        if (!okTyre) {
            log_line("replay focus: tyres: the right-eye constants copy could not be made; ACE's tyres stay left-eye only");
            focus.tyrePso.Reset();
        }
    }
    {  // R2b: the culling-constants copy
        ComPtr<ID3DBlob> cullCode, cullErrors, cullBlob;
        D3D12_ROOT_PARAMETER cullParams[3] = {};
        cullParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        cullParams[0].Descriptor = {0, 0};
        cullParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        cullParams[1].Constants = {1, 0, 22};
        cullParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        cullParams[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC cullDesc{3, cullParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC cullPso{};
        if (FAILED(D3DCompile(kCullShader, sizeof(kCullShader) - 1, "focus-culling", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                              &cullCode, &cullErrors)) ||
            FAILED(D3D12SerializeRootSignature(&cullDesc, D3D_ROOT_SIGNATURE_VERSION_1, &cullBlob, &cullErrors)) ||
            FAILED(device->CreateRootSignature(0, cullBlob->GetBufferPointer(), cullBlob->GetBufferSize(),
                                               IID_PPV_ARGS(&focus.cullRoot)))) {
            log_line("replay focus: the culling-constants shader failed; no focus tiles");
            focus.tilesBroken = true;
        } else {
            cullPso.pRootSignature = focus.cullRoot.Get();
            cullPso.CS = {cullCode->GetBufferPointer(), cullCode->GetBufferSize()};
            if (FAILED(device->CreateComputePipelineState(&cullPso, IID_PPV_ARGS(&focus.cullPso))))
                focus.tilesBroken = true;
        }
    }
    if (focus.settings.fog) {  // R2c: the fog-constants copy
        ComPtr<ID3DBlob> fogCode, fogErrors, fogBlob;
        D3D12_ROOT_PARAMETER fogParams[3] = {};
        fogParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        fogParams[0].Descriptor = {0, 0};
        fogParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        fogParams[1].Constants = {1, 0, 9};
        fogParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        fogParams[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC fogDesc{3, fogParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC fogPso{};
        D3D12_HEAP_PROPERTIES fogHeap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC fogBuffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kFogSlots} * kFogSlotBytes, 1, 1, 1,
                                            DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        if (FAILED(D3DCompile(kFogShader, sizeof(kFogShader) - 1, "focus-fog", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                              &fogCode, &fogErrors)) ||
            FAILED(D3D12SerializeRootSignature(&fogDesc, D3D_ROOT_SIGNATURE_VERSION_1, &fogBlob, &fogErrors)) ||
            FAILED(device->CreateRootSignature(0, fogBlob->GetBufferPointer(), fogBlob->GetBufferSize(),
                                               IID_PPV_ARGS(&focus.fogRoot)))) {
            logf("replay focus: fog: the constants shader failed (%s); no focus fog",
                 fogErrors ? static_cast<const char*>(fogErrors->GetBufferPointer()) : "?");
            focus.fogBroken = true;
        } else {
            fogPso.pRootSignature = focus.fogRoot.Get();
            fogPso.CS = {fogCode->GetBufferPointer(), fogCode->GetBufferSize()};
            if (FAILED(device->CreateComputePipelineState(&fogPso, IID_PPV_ARGS(&focus.fogPso))) ||
                FAILED(device->CreateCommittedResource(&fogHeap, D3D12_HEAP_FLAG_NONE, &fogBuffer,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&focus.fogConstants))))
                focus.fogBroken = true;
        }
    }
    if (focus.settings.shadows) {  // R2c: the deferred-shadow constants copy
        ComPtr<ID3DBlob> code2, errors2, blob2;
        D3D12_ROOT_PARAMETER p2[4] = {};
        p2[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p2[0].Descriptor = {0, 0};
        p2[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p2[1].Descriptor = {1, 0};
        p2[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p2[2].Constants = {2, 0, 12};
        p2[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p2[3].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC d2{4, p2, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso2{};
        D3D12_HEAP_PROPERTIES heap2{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf2{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kShadowSlots} * kShadowSlotBytes, 1, 1, 1,
                                       DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        if (FAILED(D3DCompile(kShadowShader, sizeof(kShadowShader) - 1, "focus-shadow", nullptr, nullptr, "main", "cs_5_0", 0,
                              0, &code2, &errors2)) ||
            FAILED(D3D12SerializeRootSignature(&d2, D3D_ROOT_SIGNATURE_VERSION_1, &blob2, &errors2)) ||
            FAILED(device->CreateRootSignature(0, blob2->GetBufferPointer(), blob2->GetBufferSize(),
                                               IID_PPV_ARGS(&focus.shadowPatchRoot)))) {
            logf("replay focus: shadow: the constants shader failed (%s); no focus shadow",
                 errors2 ? static_cast<const char*>(errors2->GetBufferPointer()) : "?");
            focus.shadowBroken = true;
        } else {
            pso2.pRootSignature = focus.shadowPatchRoot.Get();
            pso2.CS = {code2->GetBufferPointer(), code2->GetBufferSize()};
            if (FAILED(device->CreateComputePipelineState(&pso2, IID_PPV_ARGS(&focus.shadowPatchPso))) ||
                FAILED(device->CreateCommittedResource(&heap2, D3D12_HEAP_FLAG_NONE, &buf2,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&focus.shadowConstants))))
                focus.shadowBroken = true;
        }
    }
    if (focus.settings.atmosphere || focus.settings.post || focus.settings.transparents) {  // atmosphere, veil, blur
        ComPtr<ID3DBlob> blob3, errors3;
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
        D3D12_ROOT_PARAMETER p3[3] = {};
        p3[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p3[0].Constants = {0, 0, 13};
        p3[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p3[1].DescriptorTable = {1, &ranges[0]};
        p3[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p3[2].DescriptorTable = {1, &ranges[1]};
        D3D12_STATIC_SAMPLER_DESC lin{};
        lin.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        lin.AddressU = lin.AddressV = lin.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lin.MaxLOD = D3D12_FLOAT32_MAX;
        lin.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const D3D12_ROOT_SIGNATURE_DESC d3{3, p3, 1, &lin, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        bool ok3 = SUCCEEDED(D3D12SerializeRootSignature(&d3, D3D_ROOT_SIGNATURE_VERSION_1, &blob3, &errors3)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob3->GetBufferPointer(), blob3->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.atmoRoot)));
        for (int dim = 0; dim < 2 && ok3; ++dim) {
            const D3D_SHADER_MACRO defines[] = {{"ATMO_3D", dim ? "1" : "0"}, {nullptr, nullptr}};
            ComPtr<ID3DBlob> code3;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso3{};
            ok3 = SUCCEEDED(D3DCompile(kAtmoShader, sizeof(kAtmoShader) - 1, "focus-atmosphere", defines, nullptr, "main",
                                       "cs_5_0", 0, 0, &code3, &errors3));
            if (ok3) {
                pso3.pRootSignature = focus.atmoRoot.Get();
                pso3.CS = {code3->GetBufferPointer(), code3->GetBufferSize()};
                ok3 = SUCCEEDED(device->CreateComputePipelineState(&pso3, IID_PPV_ARGS(&focus.atmoPso[dim])));
            }
        }
        if (!ok3) {
            logf("replay focus: atmosphere: the resample shader failed (%s); no focus atmosphere",
                 errors3 ? static_cast<const char*>(errors3->GetBufferPointer()) : "?");
            focus.atmoBroken = true;
        }
    }
    if (focus.settings.post || focus.settings.sky) {  // P1: the constant-buffer patch (tonemap; the sky's cbSky)
        ComPtr<ID3DBlob> code4, errors4, blob4;
        D3D12_ROOT_PARAMETER p4[3] = {};
        p4[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p4[0].Descriptor = {0, 0};
        p4[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p4[1].Constants = {1, 0, 8};
        p4[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p4[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC d4{3, p4, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso4{};
        D3D12_HEAP_PROPERTIES heap4{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf4{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kPostSlots} * 256, 1, 1, 1,
                                       DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        bool ok4 = SUCCEEDED(D3DCompile(kCbPatchShader, sizeof(kCbPatchShader) - 1, "focus-cb-patch", nullptr, nullptr,
                                        "main", "cs_5_0", 0, 0, &code4, &errors4)) &&
                   SUCCEEDED(D3D12SerializeRootSignature(&d4, D3D_ROOT_SIGNATURE_VERSION_1, &blob4, &errors4)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob4->GetBufferPointer(), blob4->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.cbPatchRoot)));
        if (ok4) {
            pso4.pRootSignature = focus.cbPatchRoot.Get();
            pso4.CS = {code4->GetBufferPointer(), code4->GetBufferSize()};
            ok4 = SUCCEEDED(device->CreateComputePipelineState(&pso4, IID_PPV_ARGS(&focus.cbPatchPso))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap4, D3D12_HEAP_FLAG_NONE, &buf4,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&focus.postConstants)));
        }
        if (!ok4) {
            logf("replay focus: post: the constants patch failed (%s); no focus post",
                 errors4 ? static_cast<const char*>(errors4->GetBufferPointer()) : "?");
            focus.postBroken = true;
        }
    }
    if (focus.settings.dlss) {  // P2a: the packing constants
        ComPtr<ID3DBlob> code6, errors6, blob6;
        D3D12_ROOT_PARAMETER p6[3] = {};
        p6[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p6[0].Descriptor = {0, 0};
        p6[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p6[1].Constants = {1, 0, 18};
        p6[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p6[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC d6{3, p6, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES heap6{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf6{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kPostSlots} * 512, 1, 1, 1,
                                       DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        bool ok6 = SUCCEEDED(D3DCompile(kPackShader, sizeof(kPackShader) - 1, "focus-pack", nullptr, nullptr, "main",
                                        "cs_5_0", 0, 0, &code6, &errors6)) &&
                   SUCCEEDED(D3D12SerializeRootSignature(&d6, D3D_ROOT_SIGNATURE_VERSION_1, &blob6, &errors6)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob6->GetBufferPointer(), blob6->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.packRoot)));
        if (ok6) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso6{};
            pso6.pRootSignature = focus.packRoot.Get();
            pso6.CS = {code6->GetBufferPointer(), code6->GetBufferSize()};
            ok6 = SUCCEEDED(device->CreateComputePipelineState(&pso6, IID_PPV_ARGS(&focus.packPso))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap6, D3D12_HEAP_FLAG_NONE, &buf6,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&focus.packConstants)));
        }
        if (!ok6) {
            logf("replay focus: dlss: the packing constants shader failed (%s); no focus DLSS",
                 errors6 ? static_cast<const char*>(errors6->GetBufferPointer()) : "?");
            focus.dlssBroken = true;
        }
    }
    if (focus.settings.post) {  // P2c: the vrAntialiasing constants
        ComPtr<ID3DBlob> code7, errors7, blob7;
        D3D12_ROOT_PARAMETER p7[3] = {};
        p7[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p7[0].Descriptor = {0, 0};
        p7[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p7[1].Constants = {1, 0, 9};
        p7[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p7[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC d7{3, p7, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES heap7{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf7{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kPostSlots} * 512, 1, 1, 1,
                                       DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        bool ok7 = SUCCEEDED(D3DCompile(kAaShader, sizeof(kAaShader) - 1, "focus-aa", nullptr, nullptr, "main", "cs_5_0",
                                        0, 0, &code7, &errors7)) &&
                   SUCCEEDED(D3D12SerializeRootSignature(&d7, D3D_ROOT_SIGNATURE_VERSION_1, &blob7, &errors7)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob7->GetBufferPointer(), blob7->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.aaPatchRoot)));
        if (ok7) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso7{};
            pso7.pRootSignature = focus.aaPatchRoot.Get();
            pso7.CS = {code7->GetBufferPointer(), code7->GetBufferSize()};
            ok7 = SUCCEEDED(device->CreateComputePipelineState(&pso7, IID_PPV_ARGS(&focus.aaPatchPso))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap7, D3D12_HEAP_FLAG_NONE, &buf7,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&focus.aaConstants)));
        }
        if (ok7 && focus.settings.atmosphere == 4) {  // the focus ray grid (same root signature)
            ComPtr<ID3DBlob> code8, errors8;
            const D3D12_RESOURCE_DESC buf8{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 16 * 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                           {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso8{};
            pso8.pRootSignature = focus.aaPatchRoot.Get();
            if (SUCCEEDED(D3DCompile(kAtmoGridShader, sizeof(kAtmoGridShader) - 1, "focus-atmo-grid", nullptr, nullptr,
                                     "main", "cs_5_0", 0, 0, &code8, &errors8))) {
                pso8.CS = {code8->GetBufferPointer(), code8->GetBufferSize()};
                if (FAILED(device->CreateComputePipelineState(&pso8, IID_PPV_ARGS(&focus.atmoGridPso))) ||
                    FAILED(device->CreateCommittedResource(&heap7, D3D12_HEAP_FLAG_NONE, &buf8,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                           IID_PPV_ARGS(&focus.atmoGridConstants))))
                    focus.atmoGridPso.Reset();
            }
            if (!focus.atmoGridPso)
                log_line("replay focus: atmosphere: the ray-grid shader failed; the maps are resampled instead");
        }
        if (!ok7) {
            logf("replay focus: msaa: the antialiasing constants shader failed (%s); no focus MSAA resolve",
                 errors7 ? static_cast<const char*>(errors7->GetBufferPointer()) : "?");
            focus.aaBroken = true;
        }
    }
    if (focus.settings.sky) {  // the sky's clouds resample
        ComPtr<ID3DBlob> code5, errors5, blob5;
        D3D12_DESCRIPTOR_RANGE r5[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kBindlessRange, 0, 1, 0},
                                        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
        D3D12_ROOT_PARAMETER p5[4] = {};
        p5[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p5[0].Descriptor = {0, 0};
        p5[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p5[1].Constants = {1, 0, 5};
        p5[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p5[2].DescriptorTable = {1, &r5[0]};
        p5[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p5[3].DescriptorTable = {1, &r5[1]};
        D3D12_STATIC_SAMPLER_DESC lin5{};
        lin5.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        lin5.AddressU = lin5.AddressV = lin5.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lin5.MaxLOD = D3D12_FLOAT32_MAX;
        lin5.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const D3D12_ROOT_SIGNATURE_DESC d5{4, p5, 1, &lin5, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso5{};
        D3D12_HEAP_PROPERTIES heap5{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf5{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kSkySlots} * 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                       {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        const D3D12_RESOURCE_DESC tex5{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kSkyCloudSize, kSkyCloudSize, 1, 1,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        bool ok5 = SUCCEEDED(D3DCompile(kSkyCloudShader, sizeof(kSkyCloudShader) - 1, "focus-sky-clouds", nullptr, nullptr,
                                        "main", "cs_5_1", D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &code5,
                                        &errors5)) &&
                   SUCCEEDED(D3D12SerializeRootSignature(&d5, D3D_ROOT_SIGNATURE_VERSION_1, &blob5, &errors5)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob5->GetBufferPointer(), blob5->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.skyRoot)));
        if (ok5) {
            pso5.pRootSignature = focus.skyRoot.Get();
            pso5.CS = {code5->GetBufferPointer(), code5->GetBufferSize()};
            ok5 = SUCCEEDED(device->CreateComputePipelineState(&pso5, IID_PPV_ARGS(&focus.skyPso))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap5, D3D12_HEAP_FLAG_NONE, &buf5,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&focus.skyConstants))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap5, D3D12_HEAP_FLAG_NONE, &tex5,
                                                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                            IID_PPV_ARGS(&focus.skyClouds)));
        }
        if (!ok5) {
            logf("replay focus: sky: the clouds shader failed (%s); no focus sky",
                 errors5 ? static_cast<const char*>(errors5->GetBufferPointer()) : "?");
            focus.skyBroken = true;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvSlots};
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvSlots};
    return SUCCEEDED(device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&focus.rtvHeap))) &&
           SUCCEEDED(device->CreateDescriptorHeap(&dsvHeap, IID_PPV_ARGS(&focus.dsvHeap)));
}

D3D12_CPU_DESCRIPTOR_HANDLE view_slot(bool depth, int& index) {
    std::vector<int>& free = depth ? focus.freeDsv : focus.freeRtv;
    if (free.empty())
        return {0};
    index = free.back();
    free.pop_back();
    D3D12_CPU_DESCRIPTOR_HANDLE h = (depth ? focus.dsvHeap : focus.rtvHeap)->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) *
             device->GetDescriptorHandleIncrementSize(depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return h;
}

namespace {
void free_slot(bool depth, int& index) {
    if (index >= 0)
        (depth ? focus.freeDsv : focus.freeRtv).push_back(index);
    index = -1;
}

// One twin resource and its view, `width` x `height`, the game target's format and samples.
ComPtr<ID3D12Resource> make_twin(const Twin& t, UINT width, UINT height, D3D12_CPU_DESCRIPTOR_HANDLE& view, int& slot) {
    view = view_slot(t.depth, slot);
    if (!view.ptr)
        return nullptr;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const UINT samples = t.gameDesc.SampleDesc.Count;
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, t.gameDesc.Format,
                             {samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             t.depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_CLEAR_VALUE clear{};
    clear.Format = t.viewFormat;
    if (t.depth)
        clear.DepthStencil = {0.0f, 0};  // ACE: reverse-Z
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            t.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&r))))
        return nullptr;
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC v{};
        v.Format = t.viewFormat;
        v.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvOriginal(device.Get(), r.Get(), &v, view);
    } else {
        D3D12_RENDER_TARGET_VIEW_DESC v{};
        v.Format = t.viewFormat;
        v.ViewDimension = samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvOriginal(device.Get(), r.Get(), &v, view);
    }
    return r;
}
}  // namespace

// Under focus.lock: kept alive until no list can still use it. Replays recorded earlier in a frame run later in that
// frame (and into the next), each batch with its own fence value, so "the next fence value" is too soon: a rebuild
// retires everything at once, and freeing it after the next batch left later batches reading freed textures (the
// 05:41 system crash, two seconds after a rebuild). An object waits four frames, then for every fence value signalled
// by that time.
void retire(IUnknown* object) {
    if (object)
        focus.retired.push_back({focus.frame.load(), 0, object, nullptr});
}
// Our NGX features go the same way (they were left to NGX's shutdown: two a rebuild, 112 in one session).
void retire_feature(focus_ngx::Feature*& feature) {
    if (feature)
        focus.retired.push_back({focus.frame.load(), 0, nullptr, feature});
    feature = nullptr;
}

namespace {
// Under focus.lock: releases what the GPU is done with.
void prune_retired() {
    if (!focus.fence)  // not made (focus_compile failed): nothing was retired against it
        return;
    const uint64_t done = focus.fence->GetCompletedValue(), frame = focus.frame.load();
    for (Focus::Retired& r : focus.retired)
        if (!r.fence && frame >= r.frame + 4)
            r.fence = std::max<uint64_t>(1, focus.fenceValue.load());
    focus.retired.erase(std::remove_if(focus.retired.begin(), focus.retired.end(),
                                       [done](Focus::Retired& r) {
                                           if (!r.fence || r.fence > done)
                                               return false;
                                           if (r.feature)
                                               focus_ngx::release(r.feature);
                                           return true;
                                       }),
                        focus.retired.end());
}

// ---- The focus mask (replay_probe_set_focus_mask; quad/focus_mask.h) ----
// The settings, set from any thread; the clear lists are recorded again (under focus.lock) when their epoch moves.
struct MaskSettings {
    SRWLOCK lock = SRWLOCK_INIT;
    bool on = false;
    FocusArea area{};
    unsigned margin = 16;
    std::atomic<uint64_t> epoch{1};
};
MaskSettings maskSettings;

struct MaskSnapshot {
    bool on = false;
    FocusArea area{};
    unsigned margin = 16;
    uint64_t epoch = 0;
};
MaskSnapshot mask_snapshot() {
    MaskSnapshot s;
    AcquireSRWLockShared(&maskSettings.lock);
    s.on = maskSettings.on;
    s.area = maskSettings.area;
    s.margin = maskSettings.margin;
    s.epoch = maskSettings.epoch.load();
    ReleaseSRWLockShared(&maskSettings.lock);
    return s;
}

// Under focus.lock: the mask's depth-only pipeline for a depth format and sample count (made once each). Null: none.
ID3D12PipelineState* mask_pso(DXGI_FORMAT format, DXGI_SAMPLE_DESC samples) {
    if (focus.maskFailed || format == DXGI_FORMAT_UNKNOWN)
        return nullptr;
    static ComPtr<ID3DBlob> vs;
    if (!focus.maskRoot || !vs) {
        // z 0.5 at a viewport whose depth range is one value: that value.
        static const char kSource[] = "float4 main(float2 p : POSITION) : SV_Position { return float4(p, 0.5, 1); }";
        ComPtr<ID3DBlob> errors, blob;
        const D3D12_ROOT_SIGNATURE_DESC desc{0, nullptr, 0, nullptr,
                                             D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "focus-mask", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs,
                              &errors)) ||
            FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&focus.maskRoot)))) {
            focus.maskFailed = true;
            log_line("replay mask: its shader or root signature could not be made; off for the session");
            return nullptr;
        }
    }
    const uint64_t key = (uint64_t{static_cast<uint32_t>(format)} << 32) | (uint64_t{samples.Count} << 16) | samples.Quality;
    if (const auto it = focus.maskPsos.find(key); it != focus.maskPsos.end())
        return it->second.Get();
    const D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = focus.maskRoot.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = TRUE;
    d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.InputLayout = {&element, 1};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 0;  // depth only
    d.DSVFormat = format;
    d.SampleDesc = samples;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
        logf("replay mask: no pipeline for depth format %d with %u sample(s): those twins unmasked",
             static_cast<int>(format), samples.Count);
        focus.maskPsos.emplace(key, nullptr);
        return nullptr;
    }
    return focus.maskPsos.emplace(key, pso).first->second.Get();
}

struct MaskDraw {
    const Twin* twin = nullptr;
    ID3D12PipelineState* pso = nullptr;
    UINT first = 0, count = 0;
};

// Under focus.lock: each depth twin's mesh (its focus view's size and aspect) in one upload buffer of ours.
std::vector<MaskDraw> mask_draws(const MaskSnapshot& snap) {
    std::vector<MaskDraw> draws;
    focus.maskTwins = focus.maskVertices = 0;
    focus.maskShare = 0;
    if (!snap.on || (!snap.area.outline() && snap.area.radius <= 0))
        return draws;
    std::vector<float> all;
    UINT widest = 0;
    for (const auto& [game, t] : focus.twins) {
        if (!t.depth || !t.focusW || !t.focusH)
            continue;
        FocusArea area = snap.area;
        area.aspect = static_cast<float>(t.focusW) / static_cast<float>(t.focusH);
        const std::vector<float> mesh = focus_mask_mesh(area, focus_mask_margin(snap.margin, t.focusW, t.focusH));
        ID3D12PipelineState* pso = mesh.empty() ? nullptr : mask_pso(t.viewFormat, t.gameDesc.SampleDesc);
        if (!pso)
            continue;
        draws.push_back({&t, pso, static_cast<UINT>(all.size() / 2), static_cast<UINT>(mesh.size() / 2)});
        all.insert(all.end(), mesh.begin(), mesh.end());
        if (t.focusW > widest) {  // the scene's (the largest) for the log
            widest = t.focusW;
            focus.maskShare = focus_mask_share(snap.area, t.focusW, t.focusH, snap.margin);
        }
    }
    if (all.empty())
        return {};
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, all.size() * sizeof(float), 1, 1, 1,
                                     DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> vb;
    void* data = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&vb))) ||
        FAILED(vb->Map(0, &none, &data)) || !data) {
        log_line("replay mask: its vertex buffer could not be made: unmasked");
        return {};
    }
    std::memcpy(data, all.data(), all.size() * sizeof(float));
    vb->Unmap(0, nullptr);
    retire(focus.maskVb.Get());
    focus.maskVb = vb;
    focus.maskTwins = static_cast<uint32_t>(draws.size());
    focus.maskVertices = static_cast<uint32_t>(all.size() / 2);
    return draws;
}

// Into `l` (after the clears): the mask's depth into both focus views of each depth twin (its halves).
void record_mask(List* l, const std::vector<MaskDraw>& draws) {
    if (draws.empty())
        return;
    l->SetGraphicsRootSignature(focus.maskRoot.Get());
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VERTEX_BUFFER_VIEW vbv{focus.maskVb->GetGPUVirtualAddress(),
                                       static_cast<UINT>(focus.maskVertices * 2 * sizeof(float)), 2 * sizeof(float)};
    l->IASetVertexBuffers(0, 1, &vbv);
    for (const MaskDraw& d : draws) {
        const Twin& t = *d.twin;
        l->OMSetRenderTargets(0, nullptr, FALSE, &t.focusView);
        l->SetPipelineState(d.pso);
        for (UINT e = 0; e < 2; ++e) {
            const float x = static_cast<float>(e * t.focusW);
            const D3D12_VIEWPORT vp{x, 0, static_cast<float>(t.focusW), static_cast<float>(t.focusH), 1, 1};  // nearest
            const D3D12_RECT rect{static_cast<LONG>(e * t.focusW), 0, static_cast<LONG>((e + 1) * t.focusW),
                                  static_cast<LONG>(t.focusH)};
            l->RSSetViewports(1, &vp);
            l->RSSetScissorRects(1, &rect);
            l->DrawInstanced(d.count, 1, d.first, 0);
        }
    }
}

// Under focus.lock: the clear lists for the focus twins (every frame; with the mask, and without it for the A/B) and one
// for the eye references (sample frames): depth to 0 (reverse-Z), colour to 0.
bool record_clear() {
    ComPtr<ID3D12CommandAllocator>* allocators[3] = {&focus.clearAllocator, &focus.eyeClearAllocator,
                                                     &focus.plainClearAllocator};
    ComPtr<List>* lists[3] = {&focus.clearList, &focus.eyeClearList, &focus.plainClearList};
    for (int i = 0; i < 3; ++i) {
        retire(allocators[i]->Get());
        retire(lists[i]->Get());
        allocators[i]->Reset();
        lists[i]->Reset();
    }
    const MaskSnapshot snap = mask_snapshot();
    focus.maskRecorded = snap.epoch;
    const std::vector<MaskDraw> draws = mask_draws(snap);
    for (int which = 0; which < 3; ++which) {
        ComPtr<ID3D12CommandAllocator>& a = *allocators[which];
        ComPtr<List>& l = *lists[which];
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l))))
            return false;
        inReplay = true;
        const float zero[4] = {}, one[4] = {1, 1, 1, 1};
        for (const auto& [game, t] : focus.twins) {
            const D3D12_CPU_DESCRIPTOR_HANDLE v = which == 1 ? t.eyeView : t.focusView;
            if (t.depth)
                l->ClearDepthStencilView(v, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            else  // ACE clears its OIT reveal target to 1 (nothing in front), everything else to 0
                l->ClearRenderTargetView(v, t.name == "oitReveal" ? one : zero, 0, nullptr);
        }
        if (which == 0)
            record_mask(l.Get(), draws);
        l->Close();
        inReplay = false;
    }
    if (snap.on)
        logf("replay mask: %s: %u depth twins, %u vertices, about %.0f%% of the scene's focus views (margin %u texels)",
             draws.empty() ? "nothing to mask (the plain box, or a margin past the rounding)" : "on", focus.maskTwins,
             focus.maskVertices, focus.maskShare * 100, snap.margin);
    return true;
}
}  // namespace

void focus_mask_set(bool on, const FocusArea& area, unsigned marginTexels) {
    AcquireSRWLockExclusive(&maskSettings.lock);
    const bool changed = maskSettings.on != on || !(maskSettings.area == area) || maskSettings.margin != marginTexels;
    maskSettings.on = on;
    maskSettings.area = area;
    maskSettings.margin = marginTexels;
    if (changed)
        maskSettings.epoch.fetch_add(1);
    ReleaseSRWLockExclusive(&maskSettings.lock);
}

void focus_mask_refresh() {
    if (focus.ready && maskSettings.epoch.load() != focus.maskRecorded && !record_clear())
        log_line("replay mask: the clear lists could not be recorded again");
}

int focus_mask_side(uint64_t frame) { return focus.settings.maskAb ? static_cast<int>((frame / 900) % 2) : 0; }

List* focus_clear_list(uint64_t frame) {
    return focus_mask_side(frame) == 1 && focus.plainClearList ? focus.plainClearList.Get() : focus.clearList.Get();
}

void focus_mask_window_log(uint64_t ended) {
    if (!focus.settings.maskAb || ended % 900 != 0 || ended == 0)
        return;
    logf("replay mask A/B: frames %llu-%llu were %s the focus mask (the replay gpu line just before is that side's); "
         "next %s",
         static_cast<unsigned long long>(ended - 899), static_cast<unsigned long long>(ended),
         focus_mask_side(ended - 1) == 0 ? "WITH" : "WITHOUT", focus_mask_side(ended + 1) == 0 ? "with" : "without");
}

// The rate-limited "why not" lines print again after every remake of the game's targets (remakeEpoch): a step that
// goes quiet after a mode or resolution change in the game says why.
std::atomic<int> remakeEpoch{0};

namespace {
std::atomic<uint64_t> focusRebuilds{0};
}  // namespace

bool miss_allowed(int& epoch, int& count, int limit) {
    if (const int e = remakeEpoch.load(); epoch != e) {
        epoch = e;
        count = 0;
    }
    return count++ < limit;
}

// Under focus.lock: everything the focus views sized or learned from the game's targets goes, so the next lists
// build it again exactly as at launch (a launch works in every mode; state carried across a size change did not:
// 2026-11-13/14 runs). Called when the game remakes its targets (a DLSS mode or resolution change in its menu) and
// when our own sizes change (the headset menu's focus density and Focus views mode). Pipelines, constants buffers,
// view slots of single resources, the game's view records and the bindless block's place stay.
// Under focus.lock: our records of the game's views (target views, shader views, its 3D textures) whose resource the
// game has released: only our own references keep it alive. They go, so lookups by name (the atmosphere maps, the fog
// grids) find the game's current resources and the old ones are freed. 05:29 run: after a mode change the atmosphere
// maps were found by name among stale records, ours were sized from them and resampled from them (the dark focus sky).
// Returns how many resources were let go; `used` when one of them is a resource a step found by name and still reads
// (an atmosphere map, a fog grid): that step must start over.
size_t sweep_released(bool* used = nullptr) {
    std::unordered_map<ID3D12Resource*, ULONG> ours;
    for (const auto& [h, v] : focus.views)
        if (v.resource)
            ++ours[v.resource.Get()];
    for (const auto& [h, v] : focus.srvInfo)
        if (v.first)
            ++ours[v.first.Get()];
    for (const ComPtr<ID3D12Resource>& r : focus.views3d)
        if (r)
            ++ours[r.Get()];
    for (const ComPtr<ID3D12Resource>& r : {focus.gameGrid[0], focus.gameGrid[1], focus.postGameFxaa})
        if (r)
            ++ours[r.Get()];
    std::unordered_set<ID3D12Resource*> dead;
    for (const auto& [r, count] : ours) {
        r->AddRef();
        if (r->Release() <= count && !focus.twins.count(r))  // (a twin's target goes through focus_prepare's check)
            dead.insert(r);
    }
    if (dead.empty())
        return 0;
    // Ours are the last references: dropping them here would destroy the resource at once, while replay lists
    // recorded this frame (and the ones still queued) may read it. They wait like everything else we let go of.
    for (ID3D12Resource* r : dead)
        retire(r);
    if (used) {
        *used = false;
        for (ID3D12Resource* r : focus.atmoGame)
            *used = *used || (r && dead.count(r));
        for (const ComPtr<ID3D12Resource>& r : focus.gameGrid)
            *used = *used || (r && dead.count(r.Get()));
    }
    for (int e = 0; e < 2; ++e)
        if (dead.count(focus.gameGrid[e].Get()))
            focus.gameGrid[e].Reset();
    if (dead.count(focus.postGameFxaa.Get()))
        focus.postGameFxaa.Reset();
    std::erase_if(focus.views, [&](const auto& v) { return dead.count(v.second.resource.Get()) != 0; });
    std::erase_if(focus.srvInfo, [&](const auto& v) { return dead.count(v.second.first.Get()) != 0; });
    std::erase_if(focus.views3d, [&](const ComPtr<ID3D12Resource>& r) { return dead.count(r.Get()) != 0; });
    return dead.size();
}

namespace {
// Under focus.lock. Our fixed descriptor slots (tiles, fog, shadow, atmosphere, post, DLSS...) are written again on the
// CPU once a rebuild has made their resources again, and D3D12 leaves a descriptor rewritten while a submitted list may
// still read it undefined. So after a rebuild nothing of the focus views is recorded until every replay from before it
// was submitted and the GPU is past it: checked at each list, nothing waited for (queued ones dropped after 90 frames).
void hold_slots() {
    if (!focus.slotsHeld.load(std::memory_order_relaxed))
        focus.slotsHeldFrame = focus.frame.load();
    focus.slotsFence = std::max(focus.slotsFence, focus.fenceValue.load());
    focus.slotsHeld.store(true);
}
}  // namespace

bool slots_writable() {
    if (!focus.slotsHeld.load(std::memory_order_relaxed))
        return true;
    if (!focus.pending.empty()) {
        if (focus.frame.load() - focus.slotsHeldFrame < 90)
            return false;
        size_t n = 0;
        for (auto& [list, replays] : focus.pending) {
            for (Replay* r : replays)
                r->busy = false;  // never submitted: the fence of its last run stands
            n += replays.size();
        }
        focus.pending.clear();
        focus.pendingLists.store(0, std::memory_order_relaxed);
        logf("replay focus: %zu replay(s) from before the rebuild never submitted: dropped", n);
    }
    if (focus.submitting > 0 || (focus.fence && focus.fence->GetCompletedValue() < focus.slotsFence))
        return false;
    focus.slotsHeld.store(false);
    logd("replay focus: the GPU is past every replay from before the rebuild (%llu frames): the focus views go on",
         static_cast<unsigned long long>(focus.frame.load() - focus.slotsHeldFrame));
    return true;
}

void focus_rebuild(const char* why) {
    hold_slots();
    const auto drop = [](ComPtr<ID3D12Resource>& r) {
        retire(r.Get());
        r.Reset();
    };
    for (auto& [game, t] : focus.twins) {
        retire(t.focus.Get());
        retire(t.eye.Get());
        free_slot(t.depth, t.focusSlot);
        free_slot(t.depth, t.eyeSlot);
    }
    focus.twins.clear();
    focus.refused.clear();
    focus.viewsMoved = false;
    focus.ready = false;
    // the light tiles
    for (int e = 0; e < 2; ++e) {
        drop(focus.frustums[e]);
        drop(focus.opaque[e]);
        drop(focus.transparent[e]);
    }
    focus.tilesX = focus.tilesY = 0;
    focus.depthForTiles = nullptr;
    focus.tilesShaderResource = false;
    focus.tilesReady = false;
    for (uint32_t& g : focus.gameTile)
        g = UINT32_MAX;
    for (uint32_t& g : focus.gamePrev)
        g = UINT32_MAX;
    drop(focus.prevColour);
    focus.prevSource = nullptr;
    focus.prevReady = false;
    // fog (its UAV block's place is found again with the game's UAV table)
    for (int e = 0; e < 2; ++e) {
        drop(focus.fogGrid[e]);
        drop(focus.fogHistory[e][0]);
        drop(focus.fogHistory[e][1]);
        focus.gameGrid[e].Reset();
    }
    focus.fogMade = focus.fogHistoryValid = focus.fogReady = focus.fogMissLogged = focus.fogResampleViews = false;
    focus.uavBaseIndex = focus.fogUavFirst = 0;
    // shadow
    drop(focus.normals);
    drop(focus.shadow);
    focus.shadowFw = focus.shadowFh = 0;
    focus.shadowReady = false;
    focus.sceneEyeW = 0;
    focus.sceneEyeH = 0;
    // atmosphere
    for (ComPtr<ID3D12Resource>& r : focus.atmo)
        drop(r);
    for (ID3D12Resource*& r : focus.atmoGame)
        r = nullptr;
    for (auto& eye : focus.atmoSkyWork)
        for (ComPtr<ID3D12Resource>& r : eye)
            drop(r);
    focus.atmoWorkMade = focus.atmoReplayLogged = focus.atmoMade = focus.atmoReady = focus.atmoMissLogged = false;
    focus.centresKnown = false;
    // post
    drop(focus.postVeil);
    drop(focus.postLdr);
    drop(focus.postFinal);
    drop(focus.postAlt[0]);
    drop(focus.postAlt[1]);
    focus.postGameFxaa.Reset();
    focus.postColour = nullptr;
    focus.postW = focus.postH = 0;
    focus.postReady = false;
    focus.postFrame = UINT64_MAX;  // no focus image until the post runs again: the eyes are shown as they are
    // our DLSS (the features are left to NGX's shutdown, as on a size change before)
    drop(focus.packAa);
    drop(focus.packMb);
    drop(focus.packVelocity);
    drop(focus.packFlags);
    drop(focus.dlssOut);
    drop(focus.dlssOutAuto);
    drop(focus.casOut);
    focus.packColour = focus.packVelocityTwin = focus.packFlagsTwin = nullptr;
    focus.packW = focus.packH = focus.packOutW = focus.packOutH = 0;
    for (int e = 0; e < 2; ++e) {
        retire_feature(focus.ngx[e]);
        retire_feature(focus.ngxAuto[e]);
    }
    focus.ngxW = focus.ngxH = focus.ngxOutW = focus.ngxOutH = 0;
    focus.dlssReady = false;
    focus.dlssFresh = true;
    focus.casDone = false;
    // MSAA antialiasing and resolves
    drop(focus.aaOut);
    drop(focus.aaHistory);
    focus.aaSource = nullptr;
    focus.aaReady = false;
    focus.aaFresh = true;
    drop(focus.resolvedDepth);
    drop(focus.closestDepth);
    drop(focus.resolvedVelocity);
    drop(focus.resolvedColour);
    focus.resolvedDepthTwin = Twin{};
    focus.resolvedVelocityTwin = Twin{};
    focus.resolvedColourTwin = Twin{};
    focus.resolveDepthSource = focus.resolveVelocitySource = nullptr;
    focus.resolveReady = false;
    // refraction
    drop(focus.blur);
    focus.blurColour = nullptr;
    focus.blurMips = 0;
    focus.blurReady = focus.blurMissLogged = false;
    const size_t swept = sweep_released();
    if (swept)
        logf("replay focus: %zu released game resources let go of (our records of their views)", swept);
    focus.psoFlush = 300;
    AcquireSRWLockExclusive(&focus.psoLock);
    focus.psoKinds.clear();
    ReleaseSRWLockExclusive(&focus.psoLock);
    remakeEpoch.fetch_add(1);
    focusRebuilds.fetch_add(1);
    logf("replay focus: rebuilt from nothing (%s): twins, tiles, fog, shadow, atmosphere, post and DLSS are made again", why);
}

// Under focus.lock: twins for every target the list binds, each sized from its own game target (two eyes side by
// side); a twin is made again when its game target's format, size or samples change (a resolution or MSAA change in
// the game's settings).
bool focus_prepare(const Stream& s) {
    prune_retired();
    if (!slots_writable())
        return false;
    bool changed = false;
    // Game targets the game itself has released (a DLSS mode or resolution change: it makes its targets again, with
    // new views beside the old handles): only our own references keep them alive. Their twins and our views of them
    // go, so lookups by name or size find the current ones (22:21 and 22:38 runs: a stale depth twin picked for the
    // tiles, the focus replays stopped) and the game's old targets are freed.
    // Every 10 frames, not at every list: our view records hold every view the game ever made (tens of thousands of
    // shader views on a big track with traffic), and a scan per twin per list under focus.lock took ~7 ms a frame and
    // held ACE's render workers waiting on the lock (Nordschleife 10:26 run, replay_record_timing). One pass over the
    // records counts our references to every twin's game target at once.
    std::vector<ID3D12Resource*> released;
    const uint64_t frameNow = focus.frame.load(std::memory_order_relaxed);
    if (!focus.twins.empty() && (focus.releaseChecked == UINT64_MAX || frameNow - focus.releaseChecked >= 10)) {
        focus.releaseChecked = frameNow;
        std::vector<std::pair<ID3D12Resource*, ULONG>> ours;  // a twin's game target, our references to it
        ours.reserve(focus.twins.size());
        for (const auto& [game, t] : focus.twins)
            ours.emplace_back(game, 0);
        const auto count = [&ours](ID3D12Resource* r) {
            for (auto& [game, n] : ours)
                if (game == r) {
                    ++n;
                    return;
                }
        };
        for (const auto& [h, v] : focus.views)
            count(v.resource.Get());
        for (const auto& [h, v] : focus.srvInfo)  // our record of the game's shader views of it
            count(v.first.Get());
        for (const ComPtr<ID3D12Resource>& r : {focus.gameGrid[0], focus.gameGrid[1], focus.postGameFxaa})
            count(r.Get());
        for (const auto& [game, n] : ours) {
            game->AddRef();
            const ULONG others = game->Release();  // the references left: ours and the game's
            if (!n || others > n)
                continue;
            const Twin& t = focus.twins.at(game);
            logd("replay focus: the game released its %s (%llux%u)", t.name.c_str(),
                 static_cast<unsigned long long>(t.gameDesc.Width), t.gameDesc.Height);
            released.push_back(game);
        }
    }
    if (!released.empty()) {
        for (ID3D12Resource* game : released) {  // our records of its views go (which frees the target)
            std::erase_if(focus.views, [&](const auto& v) { return v.second.resource.Get() == game; });
            std::erase_if(focus.srvInfo, [&](const auto& v) { return v.second.first.Get() == game; });
        }
        twinsRemade.fetch_add(released.size());
        focus_rebuild("the game made its targets again");
        changed = true;
        if (!slots_writable())
            return false;
    }
    if (focus.viewsMoved) {  // the game made targets again: twins of resources no view names any more are retired
        focus.viewsMoved = false;
        for (auto t = focus.twins.begin(); t != focus.twins.end();) {
            bool named = false;
            for (const auto& [h, v] : focus.views)
                if (v.resource.Get() == t->first) {
                    named = true;
                    break;
                }
            if (named) {
                ++t;
                continue;
            }
            logf("replay focus: the game made its %s again: the old twins are retired", t->second.name.c_str());
            retire(t->second.focus.Get());
            retire(t->second.eye.Get());
            free_slot(t->second.depth, t->second.focusSlot);
            free_slot(t->second.depth, t->second.eyeSlot);
            t = focus.twins.erase(t);
            twinsRemade.fetch_add(1);
            changed = true;
        }
    }
    for (const SIZE_T h : s.targets) {
        const auto v = focus.views.find(h);
        if (v == focus.views.end()) {
            if (std::find(focus.refused.begin(), focus.refused.end(), h) == focus.refused.end()) {
                focus.refused.push_back(h);
                log_line("replay focus: a stereo list binds a target view created before the probe; that list is not replayed");
            }
            static int epoch = 0, count = 0;
            if (miss_allowed(epoch, count, 4))
                logf("replay focus: a list binds a target view (handle %llx) with no record of ours; not replayed (%zu refused)",
                     static_cast<unsigned long long>(h), focus.refused.size());
            return false;
        }
        const auto t = focus.twins.find(v->second.resource.Get());
        if (t == focus.twins.end())
            continue;
        const D3D12_RESOURCE_DESC d = v->second.resource->GetDesc();
        if (d.Format != t->second.gameDesc.Format || d.SampleDesc.Count != t->second.gameDesc.SampleDesc.Count ||
            d.Width != t->second.gameDesc.Width || d.Height != t->second.gameDesc.Height) {
            logf("replay focus: the game's %s changed (%llux%u -> %llux%u, format %d -> %d, %u -> %u sample(s)): its twins "
                 "are made again",
                 t->second.name.c_str(), static_cast<unsigned long long>(t->second.gameDesc.Width), t->second.gameDesc.Height,
                 static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(t->second.gameDesc.Format),
                 static_cast<int>(d.Format), t->second.gameDesc.SampleDesc.Count, d.SampleDesc.Count);
            retire(t->second.focus.Get());
            retire(t->second.eye.Get());
            free_slot(t->second.depth, t->second.focusSlot);
            free_slot(t->second.depth, t->second.eyeSlot);
            focus.twins.erase(t);
            twinsRemade.fetch_add(1);
            changed = true;
        }
    }
    const double k = focus.sizeK;
    for (const SIZE_T h : s.targets) {
        const ViewInfo& v = focus.views[h];
        if (focus.twins.count(v.resource.Get()))
            continue;
        // A new game target with the name of an older one of another sample count (MSAA switched live): the older
        // twins go, so lookups by name find the current ones.
        const std::string newName = debug_name(v.resource.Get());
        const D3D12_RESOURCE_DESC nd = v.resource->GetDesc();
        for (auto it = focus.twins.begin(); it != focus.twins.end();) {
            Twin& o = it->second;
            // (only another sample count: the game keeps same-named targets of different sizes side by side)
            if (o.name == newName && o.depth == v.depth && o.gameDesc.SampleDesc.Count != nd.SampleDesc.Count) {
                logf("replay focus: the game made %s again (%u -> %u sample(s), %llux%u -> %llux%u): its older twins go",
                     newName.c_str(), o.gameDesc.SampleDesc.Count, nd.SampleDesc.Count,
                     static_cast<unsigned long long>(o.gameDesc.Width), o.gameDesc.Height,
                     static_cast<unsigned long long>(nd.Width), nd.Height);
                retire(o.focus.Get());
                retire(o.eye.Get());
                free_slot(o.depth, o.focusSlot);
                free_slot(o.depth, o.eyeSlot);
                it = focus.twins.erase(it);
                twinsRemade.fetch_add(1);
                changed = true;
            } else {
                ++it;
            }
        }
        Twin t;
        t.gameDesc = v.resource->GetDesc();
        t.viewFormat = v.viewFormat != DXGI_FORMAT_UNKNOWN ? v.viewFormat : t.gameDesc.Format;
        t.depth = v.depth;
        t.name = debug_name(v.resource.Get());
        t.eyeW = static_cast<UINT>(t.gameDesc.Width / 2);
        t.eyeH = t.gameDesc.Height;
        t.focusW = std::max(16u, static_cast<UINT>(std::lround(t.eyeW * k)));
        t.focusH = std::max(16u, static_cast<UINT>(std::lround(t.eyeH * focus.sizeKy)));
        t.focus = make_twin(t, t.focusW * 2, t.focusH, t.focusView, t.focusSlot);
        t.eye = make_twin(t, t.eyeW * 2, t.eyeH, t.eyeView, t.eyeSlot);
        if (!t.focus || !t.eye) {
            free_slot(t.depth, t.focusSlot);
            free_slot(t.depth, t.eyeSlot);
            logf("replay focus: could not make twins of %s", t.name.c_str());
            return false;
        }
        logd("replay focus: twin of %s (%s, format %d, %u sample(s)): focus %ux%u per eye (box %.0f%%, %.2f focus pixels "
             "per eye pixel), reference %ux%u per eye",
             t.name.c_str(), t.depth ? "depth" : "colour", static_cast<int>(t.gameDesc.Format), t.gameDesc.SampleDesc.Count,
             t.focusW, t.focusH, focus.settings.boxFraction * 100, focus.settings.pixelScale, t.eyeW, t.eyeH);
        focus.twins.emplace(v.resource.Get(), std::move(t));
        twinsMade.fetch_add(1);
        changed = true;
    }
    if ((changed || !focus.clearList) && !record_clear())
        return false;
    focus.ready = true;
    return true;
}
// Under focus.lock: a free replay entry (its last execution done).
Replay* acquire_replay() {
    if (!focus.fence)
        return nullptr;
    const uint64_t done = focus.fence->GetCompletedValue();
    for (auto& r : focus.pool)
        if (!r->busy && r->fence <= done) {
            r->busy = true;
            r->ngxLife = 0;
            return r.get();
        }
    if (focus.pool.size() >= 96) {
        static int epoch = 0, count = 0;
        if (miss_allowed(epoch, count, 3)) {
            size_t busy = 0, waiting = 0;
            for (const auto& r : focus.pool) {
                busy += r->busy ? 1 : 0;
                waiting += !r->busy && r->fence > done ? 1 : 0;
            }
            logf("replay focus: no free replay entry (%zu in the pool: %zu busy, %zu waiting on the GPU; fence done %llu of %llu)",
                 focus.pool.size(), busy, waiting, static_cast<unsigned long long>(done),
                 static_cast<unsigned long long>(focus.fenceValue.load()));
        }
        return nullptr;
    }
    auto r = std::make_unique<Replay>();
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r->allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&r->list))))
        return nullptr;
    r->list->Close();
    r->busy = true;
    focus.pool.push_back(std::move(r));
    return focus.pool.back().get();
}

// Under focus.lock: our replay `r` of the game's list, run with it at its execution.
void add_pending(List* gameList, Replay* r) {
    focus.pending[gameList].push_back(r);
    focus.pendingLists.store(focus.pending.size(), std::memory_order_relaxed);
}

// The box's half extent in NDC y (x's unless a crop or a focus height of its own makes it differ).
float focus_box_y() {
    return static_cast<float>(focus.settings.boxFractionY > 0 ? focus.settings.boxFractionY : focus.settings.boxFraction);
}
uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// Each eye's box centre in its NDC: the eye's forward axis plus the gaze offset of this frame (under focus.lock).
void focus_centres(float (&centre)[2][2]) {
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 2; ++k)
            centre[e][k] = focus.eyeCentre[e][k] + focus.gazeOffset[e][k];
}

// At the game's Close of a replayable stereo list: our replay of it, recorded now, executed after the game's batch.

void focus_record(List* gameList, const Stream& s, bool mono) {
    Replay* r = nullptr;
    std::vector<std::pair<SIZE_T, SIZE_T>> focusTargets, eyeTargets;
    std::vector<SkyPatch> skyPatches;
    // A per-eye list (mono): one eye-size viewport at x 0 (eye 0) or at the eye width (eye 1) of a two-eye target.
    const int monoEye = mono ? (s.monoX > 0 ? 1 : 0) : -1;
    if (mono)
        focus.monoTaken[monoEye].fetch_add(1, std::memory_order_relaxed);
    const bool timed = recordTiming.on;
    const int64_t tEntry = timed ? now_ticks() : 0;
    AcquireSRWLockExclusive(&focus.lock);
    const int64_t tLocked = timed ? now_ticks() : 0;
    if (focus_prepare(s)) {
        bool fits = true;
        for (const SIZE_T h : s.targets) {
            const Twin& t = focus.twins.at(focus.views.at(h).resource.Get());
            focusTargets.emplace_back(h, t.focusView.ptr);
            eyeTargets.emplace_back(h, t.eyeView.ptr);
            // (within a pixel: an odd double-wide target leaves each eye half a pixel off)
            if (mono && (std::fabs(static_cast<float>(t.eyeW) - s.monoW) > 1 || static_cast<float>(t.eyeH) != s.monoH ||
                         (s.monoX != 0 && std::fabs(s.monoX - s.monoW) > 1)))
                fits = false;  // not one eye of this target (a full-target pass, or another size)
        }
        if (fits)
            r = acquire_replay();
        if (r && !mono && (focus.settings.diagMode != 3 || (focus.samplesTaken.load() % 4) & 2))
            skyPatches = focus_sky_prepare(s);
        else if (!r && mono && !fits) {
            focus.monoUnfit.fetch_add(1, std::memory_order_relaxed);
            static int unfitEpoch = 0, unfitLogged = 0;
            if (miss_allowed(unfitEpoch, unfitLogged, 6)) {
                std::string sizes;
                for (const SIZE_T h : s.targets) {
                    const Twin& t = focus.twins.at(focus.views.at(h).resource.Get());
                    sizes += " " + t.name + " " + std::to_string(t.eyeW) + "x" + std::to_string(t.eyeH);
                }
                logd("replay focus: per-eye list x %.0f %.0fx%.0f doesn't fit its targets' eye size (%s); skipped", s.monoX,
                     s.monoW, s.monoH, sizes.c_str());
            }
        }
        if (!mono) {
            // The scene's eye size (what a per-eye list must match): only from lists that write the scene depth
            // (msaaDepth). ACE's half-width flags pass (flagsDepth) is stereo too; taking its size let per-eye lists
            // closing on other threads right after it fail the match and drop out (intermittent missing transparents).
            bool scene = false;
            for (const SIZE_T h : s.targets) {
                const Twin& t = focus.twins.at(focus.views.at(h).resource.Get());
                scene = scene || (t.depth && t.name == "msaaDepth");
            }
            if (scene) {
                focus.sceneEyeW = s.eyeW;
                focus.sceneEyeH = s.eyeH;
            }
        }
    }
    // This list's own eye size (ACE's passes differ: the flags pass is half width) and its focus size.
    const double k = focus.sizeK;
    const float sEyeW = mono ? s.monoW : s.eyeW, sEyeH = mono ? s.monoH : s.eyeH;
    const UINT ewi = static_cast<UINT>(sEyeW), ehi = static_cast<UINT>(sEyeH);
    const UINT fwi = std::max(16u, static_cast<UINT>(std::lround(sEyeW * k)));
    const UINT fhi = std::max(16u, static_cast<UINT>(std::lround(sEyeH * focus.sizeKy)));
    // R2b: the bindless base (root table 18) as the stereo scene lists set it; the tile lists' indices game -> ours.
    uint32_t tileFrom[8] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t tileTo[8] = {};
    if (s.bindlessUint.ptr && !focus.bindlessBase.ptr)
        focus.bindlessBase = s.bindlessUint;
    if (focus.tilesReady && !focus.tilesBroken)
        for (int i = 0; i < 4; ++i) {
            tileFrom[i] = focus.gameTile[i];
            tileTo[i] = focus.ourTile[i];
        }
    int pairs = 4;
    // replay_diag: sample 1 as set, 2 our fog off, 3 our 3D atmosphere maps on, 4 the sky maps too
    const uint32_t diagPhase = focus.settings.diag ? focus.samplesTaken.load() : UINT32_MAX;
    const bool atmoOn = focus.atmoReady && !focus.atmoBroken && !focus.tilesBroken &&
                        (focus.settings.diag ? diagPhase >= 2
                                             : focus.settings.atmosphere != 2 || focus.samplesTaken.load() % 2 == 0);
    // replay_diag=3 (the sky): by sample, bit 0 our sky maps, bit 1 our cbSky copy: 0 ACE's both, 1 our maps, 2 our
    // cbSky, 3 both ours
    const uint32_t skyPhase = focus.settings.diagMode == 3 ? focus.samplesTaken.load() % 4 : 3;
    const bool atmoSky = ((focus.settings.diag && diagPhase >= 3) || focus.settings.atmosphere >= 3) && (skyPhase & 1);
    const bool shadowPhase = focus.settings.shadows != 2 || focus.samplesTaken.load() % 2 == 0;  // A/B: on, off, ...
    const uint32_t shadowSrv = shadowPhase && focus.shadowReady && !focus.shadowBroken && !focus.tilesBroken
                                   ? focus.slotFirst + 15 - focus.baseIndex
                                   : 0;
    const UINT focusTilesX = focus.tilesReady && !focus.tilesBroken && focus.framePso ? focus.tilesX : 0, focusTilesY = focus.tilesY;
    // frameData's depthBindlessIndex (the particles' depth fade): our resolved focus scene depth, where slot 12 names
    // it (MSAA: the resolve replayed mid-frame rests as a shader resource; a depth twin still bound for writing can't
    // be sampled, so without MSAA the game's index stays).
    static std::atomic<bool> depthFadeLogged{false};
    const uint32_t sceneDepthSrv = focus.settings.depthFade && !(liveDiag.load(std::memory_order_relaxed) & 1) && focus.resolvedDepth && focus.resolveReady && !focus.resolveBroken &&
                                           focus.depthForTiles == focus.resolvedDepth.Get()
                                       ? focus.slotFirst + 12 - focus.baseIndex
                                       : 0;
    if (sceneDepthSrv && focusTilesX && !depthFadeLogged.exchange(true))
        logf("replay focus: depth fade: the focus replays' frame constants name our resolved scene depth (bindless %u) "
             "for ACE's particles (spray, smoke)",
             sceneDepthSrv);
    float gazeNow[4], gazeBefore[4];  // this frame's box centres off the eyes' axes, and last frame's
    std::memcpy(gazeNow, focus.gazeOffset, sizeof(gazeNow));
    std::memcpy(gazeBefore, focus.gazePrev, sizeof(gazeBefore));
    const uint32_t fogSrv = focus.fogMade && focus.fogReady && !focus.fogBroken && !focus.tilesBroken && diagPhase != 1
                                ? focus.slotFirst + kFogFirst + 1 - focus.baseIndex
                                : 0;
    if (focus.prevReady && !focus.tilesBroken)
        for (int i = 0; i < 4; ++i)
            if (focus.gamePrev[i] != UINT32_MAX && pairs < 8) {
                tileFrom[pairs] = focus.gamePrev[i];
                tileTo[pairs++] = focus.slotFirst + 13 - focus.baseIndex;
            }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    const int64_t start = now_ticks();
    if (timed) {
        recordTiming.lockWait.fetch_add(static_cast<uint64_t>(tLocked - tEntry), std::memory_order_relaxed);
        recordTiming.underLock.fetch_add(static_cast<uint64_t>(start - tLocked), std::memory_order_relaxed);
    }
    const bool sample = focus.sampleFrame.load() == focus.frame.load();
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, mono ? kStepMono : kStepScene);
    if (!skyPatches.empty()) {  // the sky's clouds and cbSky copies, before the draws
        const float c0[2] = {focus.eyeCentre[0][0] + focus.gazeOffset[0][0], focus.eyeCentre[0][1] + focus.gazeOffset[0][1]};
        focus_sky_record(l, skyPatches, c0);
    }
    std::vector<std::pair<UINT64, UINT64>> skyMap;
    for (const SkyPatch& sp : skyPatches)
        skyMap.emplace_back(sp.gameTable, sp.ourTable);
    // 1. The focus constants: one slot per distinct root-1 address.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> slots;
    l->SetComputeRootSignature(focus.patchRoot.Get());
    l->SetPipelineState(focus.patchPso.Get());
    uint32_t params[kPatchParams] = {};
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    const float floats[4] = {box, boxY, static_cast<float>(fwi), static_cast<float>(fhi)};
    std::memcpy(params, floats, sizeof(floats));
    std::memcpy(params + 4, tileFrom, sizeof(tileFrom));
    std::memcpy(params + 12, tileTo, sizeof(tileTo));
    params[20] = fogSrv;
    params[21] = shadowSrv;
    // The sky maps (m % 4 = 2, 3) stay ACE's: the sky shader doesn't read them at the eye's screen position (10:23 run:
    // resampled, the focus sky came out uniformly darker).
    for (int m = 0; m < 8; ++m)
        params[24 + m] = atmoOn && (m % 4 < 2 || atmoSky) ? focus.slotFirst + 40 + static_cast<UINT>(m) - focus.baseIndex : 0;
    params[32] = focus.blurReady && !focus.blurBroken && !focus.tilesBroken
                     ? focus.slotFirst + 62 - focus.baseIndex
                     : 0;
    std::memcpy(params + 36, gazeNow, sizeof(gazeNow));
    std::memcpy(params + 40, gazeBefore, sizeof(gazeBefore));
    params[33] = fogSrv && focus.settings.fog == 2 ? focus.slotFirst + kFogFirst + 6 + 1 - focus.baseIndex : 0;
    l->SetComputeRoot32BitConstants(1, kPatchParams, params, 0);
    for (const D3D12_GPU_VIRTUAL_ADDRESS va : s.passConstants) {
        const uint32_t slot = focus.nextSlot.fetch_add(1) % kSlots;
        if (sample && !mono) {  // the sample's eye centres come from a stereo list's constants (per-eye lists differ)
            uint32_t none = UINT32_MAX;
            focus.sampleSlot.compare_exchange_strong(none, slot);
        }
        const D3D12_GPU_VIRTUAL_ADDRESS mine = focus.constants->GetGPUVirtualAddress() + UINT64{slot} * kSlotBytes;
        l->SetComputeRootConstantBufferView(0, va);
        l->SetComputeRootUnorderedAccessView(2, mine);
        l->Dispatch(3, 1, 1);
        slots.push_back(mine);
    }
    // frameData with the focus tile grid (tileCount: the stride of our tile lists), one copy per root-0 address.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> constantsFrom = s.passConstants;
    if (focusTilesX) {
        l->SetPipelineState(focus.framePso.Get());
        const uint32_t grid[3] = {focusTilesX, focusTilesY, sceneDepthSrv};
        l->SetComputeRoot32BitConstants(1, 3, grid, 0);
        for (const D3D12_GPU_VIRTUAL_ADDRESS va : s.frameConstants) {
            const uint32_t slot = focus.nextSlot.fetch_add(1) % kSlots;
            const D3D12_GPU_VIRTUAL_ADDRESS mine = focus.constants->GetGPUVirtualAddress() + UINT64{slot} * kSlotBytes;
            l->SetComputeRootConstantBufferView(0, va);
            l->SetComputeRootUnorderedAccessView(2, mine);
            l->Dispatch(2, 1, 1);
            constantsFrom.push_back(va);
            slots.push_back(mine);
        }
    }
    barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    // 2. Both focus views into the focus twins.
    const float fw = static_cast<float>(fwi), fh = static_cast<float>(fhi);
    const D3D12_VIEWPORT focusViews[2] = {{0, 0, fw, fh, 0, 1}, {fw, 0, fw, fh, 0, 1}};
    const D3D12_RECT focusRects[2] = {{0, 0, static_cast<LONG>(fwi), static_cast<LONG>(fhi)},
                                      {static_cast<LONG>(fwi), 0, static_cast<LONG>(2 * fwi), static_cast<LONG>(fhi)}};
    const UINT views = mono ? 1u : 2u;
    const D3D12_VIEWPORT* fv = mono ? &focusViews[monoEye] : focusViews;
    const D3D12_RECT* fr = mono ? &focusRects[monoEye] : focusRects;
    Substitution sub{fv, fr, views, &constantsFrom, &slots, &focusTargets};
    if (!skyMap.empty())
        sub.skyTables = &skyMap;
    sub.skip = cullProbePhase.load(std::memory_order_relaxed);
    // Focus culling's buffers hold what ACE's two-eye (dual frustum) culling makes: the stereo scene lists' draws. The
    // per-eye lists (transparents, the particles' OIT lists) draw from the same command buffer, but their commands are
    // counted by other culling dispatches we don't replay: in our buffer they keep the template's zero instances.
    // Pointed at ours, every instanced draw of a per-eye list came out empty in the focus views: no spray, no rain,
    // no fence panels or crowd (RenderDoc capture 2026-11-23: 5,397 + 468 instances in ACE's eyes, 0 in the focus
    // views), and they flashed back on the frames our culling wasn't ready. So a per-eye list keeps ACE's own buffers.
    if (!mono && cull.use.load(std::memory_order_relaxed) &&
        cull.readyFrame.load(std::memory_order_relaxed) == focus.frame.load(std::memory_order_relaxed)) {
        AcquireSRWLockShared(&focus.lock);
        if (cull.commands && cull.instances && cull.gameCommands && !cull.broken) {
            sub.argsFrom = cull.gameCommands;
            sub.argsTo = cull.commands.Get();
            sub.vbFrom = cull.gameInstancesVa.load();
            sub.vbTo = cull.instancesVa;
            sub.vbBytes = cull.instancesBytes;
        }
        ReleaseSRWLockShared(&focus.lock);
    }
    (sub.argsFrom ? cull.listsCulled : cull.listsWhole).fetch_add(1, std::memory_order_relaxed);
    l->RSSetViewports(views, fv);  // in case the list sets none itself
    l->RSSetScissorRects(views, fr);
    const int64_t tEmit = timed ? now_ticks() : 0;
    if (timed)
        skyCheckTicks = emitOps = 0;
    int64_t draws = emit(l, s, &sub);
    const int64_t tEmitted = timed ? now_ticks() : 0;
    if (timed) {
        recordTiming.setup.fetch_add(static_cast<uint64_t>(tEmit - start), std::memory_order_relaxed);
        recordTiming.emit.fetch_add(static_cast<uint64_t>(tEmitted - tEmit), std::memory_order_relaxed);
        recordTiming.skyChecks.fetch_add(skyCheckTicks, std::memory_order_relaxed);
        recordTiming.ops.fetch_add(emitOps, std::memory_order_relaxed);
        recordTiming.draws.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, draws)), std::memory_order_relaxed);
    }
    // 3. At sample frames, the same list unchanged into the eye-size references.
    if (sample && draws >= 0) {
        const float ew = static_cast<float>(ewi), eh = static_cast<float>(ehi);
        const D3D12_VIEWPORT eyeViews[2] = {{0, 0, ew, eh, 0, 1}, {ew, 0, ew, eh, 0, 1}};
        const D3D12_RECT eyeRects[2] = {{0, 0, static_cast<LONG>(ewi), static_cast<LONG>(ehi)},
                                        {static_cast<LONG>(ewi), 0, static_cast<LONG>(2 * ewi), static_cast<LONG>(ehi)}};
        const D3D12_VIEWPORT* ev = mono ? &eyeViews[monoEye] : eyeViews;
        const D3D12_RECT* er = mono ? &eyeRects[monoEye] : eyeRects;
        const Substitution eyeSub{ev, er, views, nullptr, nullptr, &eyeTargets};
        l->RSSetViewports(views, ev);
        l->RSSetScissorRects(views, er);
        if (emit(l, s, &eyeSub) < 0)
            draws = -1;
    }
    barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!skyPatches.empty())
        barrier(l, focus.skyConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const int64_t tClose = timed ? now_ticks() : 0;
    l->Close();
    inReplay = false;
    const int64_t tClosed = now_ticks();
    replayTicks.fetch_add(static_cast<uint64_t>(tClosed - start), std::memory_order_relaxed);
    if (timed) {
        recordTiming.close.fetch_add(static_cast<uint64_t>(tClosed - tClose), std::memory_order_relaxed);
        recordTiming.lists.fetch_add(1, std::memory_order_relaxed);
        const uint64_t whole = static_cast<uint64_t>(tClosed - tEntry);
        uint64_t seen = recordTiming.maxList.load(std::memory_order_relaxed);
        while (whole > seen && !recordTiming.maxList.compare_exchange_weak(seen, whole, std::memory_order_relaxed)) {
        }
    }
    AcquireSRWLockExclusive(&focus.lock);
    if (draws < 0)
        r->busy = false;  // recorded but never executed: the allocator is reset at its next use
    else
        add_pending(gameList, r);
    ReleaseSRWLockExclusive(&focus.lock);
    if (timed)  // (mostly the wait for focus.lock again)
        recordTiming.after.fetch_add(static_cast<uint64_t>(now_ticks() - tClosed), std::memory_order_relaxed);
    if (draws < 0) {
        if (mono)
            focus.monoBroken.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    listsReplayed.fetch_add(1, std::memory_order_relaxed);
    if (mono) {
        focus.monoReplays.fetch_add(1, std::memory_order_relaxed);
        focus.monoDone[monoEye].fetch_add(1, std::memory_order_relaxed);
    }
    callsReplayed.fetch_add(s.calls, std::memory_order_relaxed);
    drawsReplayed.fetch_add(static_cast<uint64_t>(draws), std::memory_order_relaxed);
}

}  // namespace simqvfr::replay_detail
