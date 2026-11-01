#include "compositor.h"

#include <cstring>
#include <utility>

#include <d3dcompiler.h>
#include <wincodec.h>

#include "../log.h"

namespace aceqv {
namespace {

constexpr char kShader[] = R"(
cbuffer Eye : register(b0) {
    float4 outerTan;  // left, right, up, down
    float4 focusTan;
    float4 rot0, rot1, rot2;  // rows, A eye frame -> B eye frame
    float4 outerSrc, focusSrc;
    float4 params;  // feather, border
    float4 borderColor;
};
Texture2D outerTex : register(t0);
Texture2D focusTex : register(t1);
SamplerState linearClamp : register(s0);
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
    V v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
// Keeps bilinear taps inside a source region (half a texel in from its edges): the views abut each
// other in ACE's target, and a shorter pair has unused rows below it.
float2 inside(float2 uv, float4 src, float2 size) {
    const float2 h = 0.5 / size;
    return clamp(uv, src.xy + h, src.xy + src.zw - h);
}
float4 PS(V v) : SV_Target {
    float2 outerSize;
    outerTex.GetDimensions(outerSize.x, outerSize.y);
    float4 color = outerTex.SampleLevel(linearClamp, inside(outerSrc.xy + v.uv * outerSrc.zw, outerSrc, outerSize), 0);
    const float3 a = float3(lerp(outerTan.x, outerTan.y, v.uv.x), lerp(outerTan.z, outerTan.w, v.uv.y), -1);
    const float3 b = float3(dot(rot0.xyz, a), dot(rot1.xyz, a), dot(rot2.xyz, a));
    const float2 t = b.xy / max(-b.z, 1e-6);
    const float2 f = float2((t.x - focusTan.x) / (focusTan.y - focusTan.x), (focusTan.z - t.y) / (focusTan.z - focusTan.w));
    // Derivatives before any branch (they are undefined inside divergent flow).
    const float2 uv = focusSrc.xy + f * focusSrc.zw;
    const float2 dx = ddx(uv), dy = ddy(uv);
    if (b.z < 0 && all(f >= 0) && all(f <= 1)) {
        float2 edge = min(f, 1 - f);
        float w = saturate(min(edge.x, edge.y) / max(params.x, 1e-4));
        w = w * w * (3 - 2 * w);
        // A supersampled focus view has several texels per output pixel: average four bilinear taps
        // spread over the pixel's footprint instead of sampling it once (which would alias).
        float2 size;
        focusTex.GetDimensions(size.x, size.y);
        float4 focus;
        if (max(length(dx * size), length(dy * size)) > 1.25) {
            focus = 0.25 * (focusTex.SampleLevel(linearClamp, inside(uv + 0.25 * (dx + dy), focusSrc, size), 0) +
                            focusTex.SampleLevel(linearClamp, inside(uv + 0.25 * (dx - dy), focusSrc, size), 0) +
                            focusTex.SampleLevel(linearClamp, inside(uv - 0.25 * (dx - dy), focusSrc, size), 0) +
                            focusTex.SampleLevel(linearClamp, inside(uv - 0.25 * (dx + dy), focusSrc, size), 0));
        } else {
            focus = focusTex.SampleLevel(linearClamp, inside(uv, focusSrc, size), 0);
        }
        color = lerp(color, focus, w);
        if (min(edge.x, edge.y) < params.y)
            color = borderColor;
    }
    return color;
}
)";

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
    device_ = device;
    queue_ = queue;
    viewFormat_ = viewFormat;

    Com<ID3DBlob> vs, ps, errors;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "quad-compose", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs,
                          &errors)) ||
        FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "quad-compose", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps,
                          &errors))) {
        logf("compositor: shader compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 36};
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

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 2,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kSlots};
    if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap_))) ||
        FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        log_line("compositor: descriptor heap or fence creation failed");
        return false;
    }
    srvIncrement_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvIncrement_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
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

    auto srv = [&](ID3D12Resource* r, UINT index) {
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = is_typeless(d.Format) ? viewFormat_ : d.Format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(r, &v, srv_cpu(index));
    };
    srv(outer_.Get(), slotIndex * 2);
    srv(focusImage, slotIndex * 2 + 1);
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
    list->SetPipelineState(pipeline_.Get());
    list->SetGraphicsRootDescriptorTable(1, srv_gpu(slotIndex * 2));
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float eyeWidth = static_cast<float>(outDesc.Width) / 2.0f;
    for (int eye = 0; eye < 2; ++eye) {
        D3D12_VIEWPORT vp{eye * eyeWidth, 0, eyeWidth, static_cast<float>(outDesc.Height), 0, 1};
        D3D12_RECT scissor{static_cast<LONG>(eye * eyeWidth), 0, static_cast<LONG>((eye + 1) * eyeWidth),
                           static_cast<LONG>(outDesc.Height)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);
        const EyeComposition& c = eyes[eye];
        float constants[36] = {};
        for (int i = 0; i < 4; ++i) {
            constants[i] = c.outerTan[i];
            constants[4 + i] = c.focusTan[i];
            constants[20 + i] = c.outerSource[i];
            constants[24 + i] = c.focusSource[i];
        }
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                constants[8 + row * 4 + col] = c.rotation[row * 3 + col];
        constants[28] = c.feather;
        constants[29] = c.border;
        for (int i = 0; i < 4; ++i)
            constants[32 + i] = c.borderColor[i];
        list->SetGraphicsRoot32BitConstants(0, 36, constants, 0);
        list->DrawInstanced(3, 1, 0, 0);
    }
    D3D12_RESOURCE_BARRIER back =
        transition(focusImage, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &back);
    return submit(*slot);
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
