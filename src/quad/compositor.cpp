#include "compositor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <d3dcompiler.h>
#include <wincodec.h>

#include "../log.h"
#include "compose_shader.h"

namespace aceqv {
namespace {

constexpr UINT kConstants = kComposeConstants;
constexpr UINT kSrvsPerSlot = 6;  // outer, focus, light, the helmet's visor, bottom and top
constexpr DXGI_FORMAT kLightFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr float kLightBlend = 0.08f;  // per composed frame: the light settles within about half a second

bool is_typeless(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return true;
    default:
        return false;
    }
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return b;
}

}  // namespace

bool QuadCompositor::initialize(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT viewFormat) {
    if (ready_)
        return true;
    if (failInitialize) {
        log_line("compositor: initialize failed (forced by a test)");
        return false;
    }
    device_ = device;
    queue_ = queue;
    viewFormat_ = viewFormat;

    Com<ID3DBlob> vs, ps, lightPs, helmetPs, errors;
    if (FAILED(D3DCompile(kComposeShader, sizeof(kComposeShader) - 1, "quad-compose", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs,
                          &errors)) ||
        FAILED(D3DCompile(kComposeShader, sizeof(kComposeShader) - 1, "quad-compose", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps,
                          &errors)) ||
        FAILED(D3DCompile(kComposeShader, sizeof(kComposeShader) - 1, "quad-compose", nullptr, nullptr, "LightPS", "ps_5_0", 0, 0,
                          &lightPs, &errors)) ||
        FAILED(D3DCompile(kComposeShader, sizeof(kComposeShader) - 1, "quad-compose", nullptr, nullptr, "HelmetLayerPS", "ps_5_0", 0, 0,
                          &helmetPs, &errors))) {
        logf("compositor: shader compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvsPerSlot, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, kConstants};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &srvRange};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsDesc{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Com<ID3DBlob> rsBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                           IID_PPV_ARGS(&rootSignature_)))) {
        log_line("compositor: root signature failed");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = rootSignature_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = viewFormat;
    pso.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_)))) {
        logf("compositor: pipeline state failed for format %d", viewFormat);
        return false;
    }
    // The helmet layer: into its own swapchain image, no blending (it carries its own alpha).
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC layer = pso;
        layer.PS = {helmetPs->GetBufferPointer(), helmetPs->GetBufferSize()};
        layer.RTVFormats[0] = kHelmetLayerFormat;
        if (FAILED(device->CreateGraphicsPipelineState(&layer, IID_PPV_ARGS(&helmetLayerPipeline_)))) {
            log_line("compositor: helmet layer pipeline failed");
            return false;
        }
    }
    // The helmet's light pass: into the light texture, blended with the last frames' by the blend factor.
    pso.PS = {lightPs->GetBufferPointer(), lightPs->GetBufferSize()};
    pso.RTVFormats[0] = kLightFormat;
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    D3D12_HEAP_PROPERTIES lightHeap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC lightDesc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 2, 1, 1, kLightFormat, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&lightPipeline_))) ||
        FAILED(device->CreateCommittedResource(&lightHeap, D3D12_HEAP_FLAG_NONE, &lightDesc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&light_)))) {
        log_line("compositor: helmet light pass setup failed");
        return false;
    }
    {  // read_light's buffer
        UINT64 total = 0;
        device->GetCopyableFootprints(&lightDesc, 0, 1, 0, &lightFootprint_, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = total;
        bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&lightReadback_))))
            lightReadback_.Reset();  // no light in the log; the overlay itself is unaffected
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * kSrvsPerSlot,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kSlots + 2};  // + the light texture, the helmet layer
    if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap_))) ||
        FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        log_line("compositor: descriptor heap or fence creation failed");
        return false;
    }
    srvIncrement_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvIncrement_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE lightRtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        lightRtv.ptr += static_cast<SIZE_T>(kSlots) * rtvIncrement_;
        device->CreateRenderTargetView(light_.Get(), nullptr, lightRtv);
    }
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (Slot& s : slots_) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&s.list)))) {
            log_line("compositor: command list creation failed");
            return false;
        }
        s.list->Close();
    }
    ready_ = true;
    logf("compositor: ready (view format %d)", viewFormat);
    return true;
}

QuadCompositor::Slot* QuadCompositor::next_slot() {
    Slot& slot = slots_[slotIndex_++ % kSlots];
    if (slot.fenceValue && fence_->GetCompletedValue() < slot.fenceValue) {
        fence_->SetEventOnCompletion(slot.fenceValue, fenceEvent_);
        WaitForSingleObject(fenceEvent_, 1000);
    }
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
        return nullptr;
    return &slot;
}

bool QuadCompositor::submit(Slot& slot) {
    if (FAILED(slot.list->Close()))
        return false;
    ID3D12CommandList* lists[] = {slot.list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    slot.fenceValue = ++fenceCounter_;
    return SUCCEEDED(queue_->Signal(fence_.Get(), slot.fenceValue));
}

D3D12_CPU_DESCRIPTOR_HANDLE QuadCompositor::srv_cpu(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * srvIncrement_;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE QuadCompositor::srv_gpu(UINT index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(index) * srvIncrement_;
    return h;
}

bool QuadCompositor::capture_outer(ID3D12Resource* appImage) {
    if (!ready_ || !appImage)
        return false;
    const D3D12_RESOURCE_DESC desc = appImage->GetDesc();
    bool fresh = false;
    if (!outer_ || outer_->GetDesc().Width != desc.Width || outer_->GetDesc().Height != desc.Height) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC outerDesc = desc;
        outerDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        outer_.Reset();
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &outerDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outer_)))) {
            log_line("compositor: outer texture creation failed");
            return false;
        }
        fresh = true;
        logf("compositor: outer texture %llux%u format %d", static_cast<unsigned long long>(desc.Width), desc.Height,
             desc.Format);
    }
    Slot* slot = next_slot();
    if (!slot)
        return false;
    D3D12_RESOURCE_BARRIER before[2] = {
        transition(appImage, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(outer_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    slot->list->ResourceBarrier(fresh ? 1 : 2, before);
    slot->list->CopyResource(outer_.Get(), appImage);
    D3D12_RESOURCE_BARRIER after[2] = {
        transition(appImage, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        transition(outer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    slot->list->ResourceBarrier(2, after);
    outerValid_ = submit(*slot);
    return outerValid_;
}

bool QuadCompositor::compose(ID3D12Resource* focusImage, ID3D12Resource* output, const EyeComposition (&eyes)[2]) {
    if (!ready_ || !outerValid_ || !focusImage || !output)
        return false;
    const UINT slotIndex = slotIndex_ % kSlots;
    Slot* slot = next_slot();
    if (!slot)
        return false;

    const UINT pieceMask = bind_sources(slotIndex, focusImage);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(slotIndex) * rtvIncrement_;
    const D3D12_RESOURCE_DESC outDesc = output->GetDesc();
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = is_typeless(outDesc.Format) ? viewFormat_ : outDesc.Format;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(output, &rtvDesc, rtv);

    ID3D12GraphicsCommandList* list = slot->list.Get();
    D3D12_RESOURCE_BARRIER toRead =
        transition(focusImage, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &toRead);
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rootSignature_.Get());
    list->SetGraphicsRootDescriptorTable(1, srv_gpu(slotIndex * kSrvsPerSlot));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const bool lightCopied =
        (eyes[0].helmet > 0 || eyes[1].helmet > 0) && record_light(list, eyes[0].outerSource, eyes[1].outerSource);
    list->SetPipelineState(pipeline_.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float eyeWidth = static_cast<float>(outDesc.Width) / 2.0f;
    for (int eye = 0; eye < 2; ++eye) {
        const EyeComposition& c = eyes[eye];
        // The FOV crop draws the eye into the top left of its half, at the same density.
        const float w = eyeWidth * (c.outputSize[0] > 0 ? c.outputSize[0] : 1.0f),
                    h = static_cast<float>(outDesc.Height) * (c.outputSize[1] > 0 ? c.outputSize[1] : 1.0f);
        D3D12_VIEWPORT vp{eye * eyeWidth, 0, w, h, 0, 1};
        D3D12_RECT scissor{static_cast<LONG>(eye * eyeWidth), 0, static_cast<LONG>(std::ceil(eye * eyeWidth + w)),
                           static_cast<LONG>(std::ceil(h))};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);
        float constants[kConstants] = {};
        compose_constants(c, eye, constants);
        for (int i = 0; i < 4; ++i)
            constants[40 + i] = c.helmetOpening[i];  // the pieces' looks: the depth shift (below)
        // Without pieces, the built-in look (and its light diagnostic); with them, the diagnostic keeps their shapes.
        const int look = c.helmet == 2 && !pieceMask ? 1 : c.helmet == 3 && pieceMask ? 4 : c.helmet;
        const bool pieceLook = look == 2 || look == 4;
        const Piece& visor = pieces_[static_cast<int>(HelmetPiece::Visor)];
        const bool tint = visor.texture && (c.visorBlend == 1 || (c.visorBlend == 0 && visor.opaque));
        constants[44] = pieceLook ? static_cast<float>(pieceMask | (tint ? 8u : 0u)) : c.helmetRim;
        constants[45] = pieceLook ? static_cast<float>(visor.whitePoint) : c.helmetRadius;
        if (pieceLook) {
            constants[40] = c.helmetShift[0];
            constants[41] = c.helmetShift[1];
        }
        constants[46] = c.helmetLight;
        constants[47] = static_cast<float>(look);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                constants[48 + row * 4 + col] = c.helmetRotation[row * 3 + col];
        constants[51] = c.helmetTanHalf[0];
        constants[55] = c.helmetTanHalf[1];
        constants[59] = c.visorOffset;
        list->SetGraphicsRoot32BitConstants(0, kConstants, constants, 0);
        list->DrawInstanced(3, 1, 0, 0);
    }
    D3D12_RESOURCE_BARRIER back =
        transition(focusImage, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &back);
    const bool ok = submit(*slot);
    if (ok && lightCopied)
        lightCopyFence_ = slot->fenceValue;
    return ok;
}

UINT QuadCompositor::bind_sources(UINT slotIndex, ID3D12Resource* focus) {
    auto srv = [&](ID3D12Resource* r, UINT index) {
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = is_typeless(d.Format) ? viewFormat_ : d.Format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(r, &v, srv_cpu(index));
    };
    srv(outer_.Get(), slotIndex * kSrvsPerSlot);
    srv(focus, slotIndex * kSrvsPerSlot + 1);
    srv(light_.Get(), slotIndex * kSrvsPerSlot + 2);
    UINT pieceMask = 0;
    for (UINT p = 0; p < 3; ++p) {
        if (pieces_[p].texture) {
            srv(pieces_[p].texture.Get(), slotIndex * kSrvsPerSlot + 3 + p);
            pieceMask |= 1u << p;
        } else {  // a null view: reads 0, never sampled without the piece
            D3D12_SHADER_RESOURCE_VIEW_DESC none = {};
            none.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            none.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(nullptr, &none, srv_cpu(slotIndex * kSrvsPerSlot + 3 + p));
        }
    }
    return pieceMask;
}

bool QuadCompositor::record_light(ID3D12GraphicsCommandList* list, const float (&source0)[4], const float (&source1)[4]) {
    bool lightCopied = false;
    {
        // The helmet's light: each eye's sides, from the outer views, blended over frames.
        D3D12_CPU_DESCRIPTOR_HANDLE lightRtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        lightRtv.ptr += static_cast<SIZE_T>(kSlots) * rtvIncrement_;
        D3D12_RESOURCE_BARRIER toTarget =
            transition(light_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &toTarget);
        list->SetPipelineState(lightPipeline_.Get());
        list->OMSetRenderTargets(1, &lightRtv, FALSE, nullptr);
        const float factor = lightFresh_ ? 1.0f : kLightBlend, factors[4] = {factor, factor, factor, factor};
        list->OMSetBlendFactor(factors);
        const D3D12_VIEWPORT lightVp{0, 0, 4, 2, 0, 1};
        const D3D12_RECT lightScissor{0, 0, 4, 2};
        list->RSSetViewports(1, &lightVp);
        list->RSSetScissorRects(1, &lightScissor);
        float constants[kConstants] = {};
        std::memcpy(&constants[20], source0, sizeof(source0));
        std::memcpy(&constants[24], source1, sizeof(source1));
        list->SetGraphicsRoot32BitConstants(0, kConstants, constants, 0);
        list->DrawInstanced(3, 1, 0, 0);
        if (lightWanted_ && lightReadback_ && !lightCopyFence_) {  // for read_light
            D3D12_RESOURCE_BARRIER toCopy =
                transition(light_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &toCopy);
            D3D12_TEXTURE_COPY_LOCATION dst{lightReadback_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = lightFootprint_;
            D3D12_TEXTURE_COPY_LOCATION src{light_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER back =
                transition(light_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &back);
            lightCopied = true;
            lightWanted_ = false;
        } else {
            D3D12_RESOURCE_BARRIER lightToRead =
                transition(light_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &lightToRead);
        }
        lightFresh_ = false;
    }
    return lightCopied;
}

bool QuadCompositor::draw_helmet_layer(ID3D12Resource* target, const HelmetLayerDraw& d) {
    if (!ready_ || !outerValid_ || !target || !has_helmet_pieces())
        return false;
    const UINT slotIndex = slotIndex_ % kSlots;
    Slot* slot = next_slot();
    if (!slot)
        return false;
    const UINT pieceMask = bind_sources(slotIndex, outer_.Get());  // no focus view here
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kSlots + 1) * rtvIncrement_;
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = kHelmetLayerFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(target, &rtvDesc, rtv);
    ID3D12GraphicsCommandList* list = slot->list.Get();
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rootSignature_.Get());
    list->SetGraphicsRootDescriptorTable(1, srv_gpu(slotIndex * kSrvsPerSlot));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const bool lightCopied = record_light(list, d.outerSource[0], d.outerSource[1]);
    list->SetPipelineState(helmetLayerPipeline_.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const D3D12_VIEWPORT vp{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
    list->RSSetViewports(1, &vp);
    const Piece& visor = pieces_[static_cast<int>(HelmetPiece::Visor)];
    const bool tint = visor.texture && (d.visorBlend == 1 || (d.visorBlend == 0 && visor.opaque));
    float constants[kConstants] = {};
    constants[0] = d.coverTan[0];
    constants[1] = d.coverTan[1];
    constants[2] = d.imageTan[0];
    constants[3] = d.imageTan[1];
    constants[4] = d.visorOffset;
    constants[44] = static_cast<float>(pieceMask | (tint ? 8u : 0u));
    constants[45] = static_cast<float>(visor.whitePoint);
    constants[46] = d.light;
    constants[47] = d.checkLight ? 4.0f : 2.0f;
    list->SetGraphicsRoot32BitConstants(0, kConstants, constants, 0);
    // Only the bands the pieces draw on (the rest of the image is never submitted).
    const std::vector<HelmetBand> whole = {{0, 1}};
    for (const HelmetBand& b : d.bands.empty() ? whole : d.bands) {
        const D3D12_RECT scissor{0, static_cast<LONG>(std::floor(b.top * desc.Height)), static_cast<LONG>(desc.Width),
                                 static_cast<LONG>(std::ceil(b.bottom * desc.Height))};
        list->RSSetScissorRects(1, &scissor);
        list->DrawInstanced(3, 1, 0, 0);
    }
    const bool ok = submit(*slot);
    if (ok && lightCopied)
        lightCopyFence_ = slot->fenceValue;
    return ok;
}

namespace {

// IEEE half to float (the light texture is R16G16B16A16_FLOAT).
float half_to_float(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ffu;
    uint32_t bits;
    if (exponent == 0)
        return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31)
        bits = sign | 0x7f800000u | (mantissa << 13);
    else
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

}  // namespace

bool QuadCompositor::read_light(float (&rgb)[2][4][3]) {
    if (!lightCopyFence_ || !fence_ || fence_->GetCompletedValue() < lightCopyFence_)
        return false;
    lightCopyFence_ = 0;
    uint8_t* mapped = nullptr;
    if (FAILED(lightReadback_->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return false;
    for (int eye = 0; eye < 2; ++eye)
        for (int side = 0; side < 4; ++side) {
            uint16_t texel[4];
            std::memcpy(texel, mapped + lightFootprint_.Offset + static_cast<size_t>(eye) * lightFootprint_.Footprint.RowPitch +
                                   static_cast<size_t>(side) * sizeof(texel),
                        sizeof(texel));
            for (int c = 0; c < 3; ++c)
                rgb[eye][side][c] = half_to_float(texel[c]);
        }
    const D3D12_RANGE nothing{0, 0};
    lightReadback_->Unmap(0, &nothing);
    return true;
}

std::vector<bool> QuadCompositor::helmet_rows(double visorOffset, int visorBlend) const {
    std::vector<bool> rows(kHelmetRows, false);
    for (const HelmetPiece p : {HelmetPiece::Top, HelmetPiece::Bottom}) {
        const Piece& piece = pieces_[static_cast<int>(p)];
        if (piece.texture)
            for (int i = 0; i < kHelmetRows; ++i)
                rows[i] = rows[i] || piece.rows[i];
    }
    const Piece& visor = pieces_[static_cast<int>(HelmetPiece::Visor)];
    if (visor.texture) {
        const bool tint = visorBlend == 1 || (visorBlend == 0 && visor.opaque);
        const std::vector<bool>& drawn = tint ? visor.tintRows : visor.rows;
        for (int i = 0; i < kHelmetRows; ++i) {  // image row v shows the visor's row v + offset (none past its ends)
            const double v = (i + 0.5) / kHelmetRows + visorOffset;
            if (v >= 0 && v < 1)
                rows[i] = rows[i] || drawn[static_cast<size_t>(v * kHelmetRows)];
        }
    }
    return rows;
}

double QuadCompositor::helmet_aspect() const {
    for (const Piece& p : pieces_)
        if (p.texture)
            return p.aspect;
    return 1;
}

bool QuadCompositor::load_helmet_piece(HelmetPiece which, const std::wstring& path) {
    if (!ready_)
        return false;
    Piece& piece = pieces_[static_cast<int>(which)];
    piece = Piece{};
    if (path.empty())
        return true;
    // Decode with WIC as straight-alpha RGBA.
    std::vector<uint8_t> pixels;
    UINT width = 0, height = 0;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Com<IWICImagingFactory> wic;
        Com<IWICBitmapDecoder> decoder;
        Com<IWICBitmapFrameDecode> frame;
        Com<IWICFormatConverter> converter;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                     &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                            0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0 && width <= 8192 && height <= 8192) {
            pixels.resize(static_cast<size_t>(width) * height * 4);
            if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())))
                pixels.clear();
        }
    }
    if (SUCCEEDED(co))
        CoUninitialize();
    if (pixels.empty()) {
        logf("compositor: helmet image %ls could not be read", path.c_str());
        return false;
    }
    // Where the piece's opaque part ends down its middle column: the top piece from its top row, the
    // bottom piece from its bottom row (for the crop hint).
    if (which != HelmetPiece::Visor) {
        const bool fromTop = which == HelmetPiece::Top;
        UINT rows = 0;
        while (rows < height) {
            const UINT y = fromTop ? rows : height - 1 - rows;
            if (pixels[(static_cast<size_t>(y) * width + width / 2) * 4 + 3] < 128)
                break;
            ++rows;
        }
        piece.edge = rows ? static_cast<double>(rows) / height : -1;
    }
    piece.aspect = static_cast<double>(height) / width;
    // Whether it has (almost) no transparency, and its white point: the linear brightness its brightest
    // 1% reach (for a visor drawn as a tint, that counts as clear).
    {
        size_t seeThrough = 0;
        size_t histogram[256] = {};
        for (size_t i = 0; i < pixels.size(); i += 4) {
            seeThrough += pixels[i + 3] < 250;
            ++histogram[std::max({pixels[i], pixels[i + 1], pixels[i + 2]})];
        }
        const size_t count = pixels.size() / 4;
        piece.opaque = seeThrough * 200 < count;
        size_t above = 0;
        int level = 255;
        for (; level > 0; --level) {
            above += histogram[level];
            if (above * 100 >= count)
                break;
        }
        const double encoded = level / 255.0;
        const double linear = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
        piece.whitePoint = std::max(0.05, linear);
    }
    // The rows it draws on (any pixel more than faintly opaque) and, as glass, the rows it tints (any pixel
    // passing less than 98.5% of the scene): the helmet layer leaves the clear ones out (helmet_bands).
    {
        double linearOf[256];  // sRGB to linear, once per value
        for (int c = 0; c < 256; ++c) {
            const double e = c / 255.0;
            linearOf[c] = e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
        }
        const auto linear = [&linearOf](uint8_t c) { return linearOf[c]; };
        piece.rows.assign(kHelmetRows, false);
        piece.tintRows.assign(kHelmetRows, false);
        for (UINT y = 0; y < height; ++y) {
            bool drawn = false, tints = false;
            for (UINT x = 0; x < width && !(drawn && tints); ++x) {
                const uint8_t* px = &pixels[(static_cast<size_t>(y) * width + x) * 4];
                if (px[3] <= 2)
                    continue;
                drawn = true;
                const double lum = 0.2126 * linear(px[0]) + 0.7152 * linear(px[1]) + 0.0722 * linear(px[2]);
                tints = tints || lum / piece.whitePoint < 0.985;
            }
            // Every bin the image row spans (several for an image shorter than kHelmetRows).
            const size_t first = static_cast<size_t>(y) * kHelmetRows / height,
                         last = std::max(first, static_cast<size_t>(y + 1) * kHelmetRows / height - 1);
            for (size_t bin = first; bin <= last && bin < kHelmetRows; ++bin) {
                piece.rows[bin] = piece.rows[bin] || drawn;
                piece.tintRows[bin] = piece.tintRows[bin] || tints;
            }
        }
    }
    // Premultiplied, so filtering never blends in the colour of transparent pixels (no fringes at the edges).
    for (size_t i = 0; i < pixels.size(); i += 4)
        for (size_t c = 0; c < 3; ++c)
            pixels[i + c] = static_cast<uint8_t>((pixels[i + c] * pixels[i + 3] + 127) / 255);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT}, upload{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
                             DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Com<ID3D12Resource> texture, buffer;
    uint8_t* mapped = nullptr;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&texture))) ||
        FAILED(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer))) ||
        FAILED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return false;
    for (UINT y = 0; y < height; ++y)
        std::memcpy(mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    &pixels[static_cast<size_t>(y) * width * 4], static_cast<size_t>(width) * 4);
    buffer->Unmap(0, nullptr);
    Slot* slot = next_slot();
    if (!slot)
        return false;
    D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = footprint;
    slot->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER ready =
        transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    slot->list->ResourceBarrier(1, &ready);
    if (!submit(*slot))
        return false;
    fence_->SetEventOnCompletion(slot->fenceValue, fenceEvent_);  // the upload buffer must outlive the copy
    WaitForSingleObject(fenceEvent_, 5000);
    piece.texture = texture;
    logf("compositor: helmet image %ls, %ux%u%s, white point %.2f", path.c_str(), width, height,
         piece.opaque ? ", opaque" : "", piece.whitePoint);
    return true;
}

bool QuadCompositor::save_left_eye(ID3D12Resource* image, D3D12_RESOURCE_STATES state, const std::wstring& path,
                                   UINT widthDivisor) {
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    const UINT width = static_cast<UINT>(desc.Width / widthDivisor), height = desc.Height;
    D3D12_RESOURCE_DESC eye = desc;
    eye.Width = width;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 total = 0;
    device_->GetCopyableFootprints(&eye, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Com<ID3D12Resource> buffer;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))))
        return false;
    Slot* slot = next_slot();
    if (!slot)
        return false;
    D3D12_RESOURCE_BARRIER toCopy = transition(image, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    slot->list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    const D3D12_BOX box{0, 0, 0, width, height, 1};
    slot->list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    D3D12_RESOURCE_BARRIER back = transition(image, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    slot->list->ResourceBarrier(1, &back);
    if (!submit(*slot))
        return false;
    fence_->SetEventOnCompletion(slot->fenceValue, fenceEvent_);
    WaitForSingleObject(fenceEvent_, 5000);

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    uint8_t* mapped = nullptr;
    if (FAILED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return false;
    for (UINT y = 0; y < height; ++y) {
        uint8_t* row = &pixels[static_cast<size_t>(y) * width * 4];
        std::memcpy(row, mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    static_cast<size_t>(width) * 4);
        for (UINT x = 0; x < width; ++x) {  // RGBA -> BGRA (what every WIC PNG encoder takes), opaque
            std::swap(row[x * 4], row[x * 4 + 2]);
            row[x * 4 + 3] = 255;
        }
    }
    buffer->Unmap(0, nullptr);

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        Com<IWICImagingFactory> wic;
        Com<IWICStream> stream;
        Com<IWICBitmapEncoder> encoder;
        Com<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
             SUCCEEDED(wic->CreateStream(&stream)) &&
             SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
             SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
             SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
             SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
             format == GUID_WICPixelFormat32bppBGRA &&
             SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data())) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (SUCCEEDED(co))
        CoUninitialize();
    return ok;
}

bool QuadCompositor::save_views4_captures(ID3D12Resource* views, ID3D12Resource* output, const std::wstring& prefix) {
    if (!ready_)
        return false;
    return save_left_eye(views, D3D12_RESOURCE_STATE_RENDER_TARGET, prefix + L"-views.png", 1) &&
           save_left_eye(output, D3D12_RESOURCE_STATE_RENDER_TARGET, prefix + L"-output.png");
}

bool QuadCompositor::save_captures(ID3D12Resource* focusImage, ID3D12Resource* output, const std::wstring& prefix) {
    if (!ready_ || !outerValid_)
        return false;
    return save_left_eye(outer_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, prefix + L"-outer.png") &&
           save_left_eye(focusImage, D3D12_RESOURCE_STATE_RENDER_TARGET, prefix + L"-focus.png") &&
           save_left_eye(output, D3D12_RESOURCE_STATE_RENDER_TARGET, prefix + L"-output.png");
}

void QuadCompositor::shutdown() {
    if (fence_ && fenceCounter_ && fence_->GetCompletedValue() < fenceCounter_) {
        fence_->SetEventOnCompletion(fenceCounter_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, 2000);
    }
    for (Slot& s : slots_)
        s = Slot{};
    outer_.Reset();
    light_.Reset();
    lightReadback_.Reset();
    lightCopyFence_ = 0;
    lightWanted_ = false;
    for (Piece& p : pieces_)
        p = Piece{};
    lightPipeline_.Reset();
    helmetLayerPipeline_.Reset();
    lightFresh_ = true;
    pipeline_.Reset();
    rootSignature_.Reset();
    srvHeap_.Reset();
    rtvHeap_.Reset();
    fence_.Reset();
    queue_.Reset();
    device_.Reset();
    if (fenceEvent_)
        CloseHandle(fenceEvent_);
    fenceEvent_ = nullptr;
    outerValid_ = ready_ = false;
}

}  // namespace aceqv
