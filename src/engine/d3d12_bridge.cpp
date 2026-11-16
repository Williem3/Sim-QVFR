#include "d3d12_bridge.h"

#include <cstdio>

#include "../log.h"

using Microsoft::WRL::ComPtr;

namespace simqvfr {

namespace {

// A released view is destroyed no sooner than this many collect() calls (frames) after its release, and only with the
// GPU through its last submission.
constexpr unsigned long long kReleaseFrames = 8;
// shutdown()'s wait for each device.
constexpr DWORD kShutdownWaitMs = 3000;

}  // namespace

bool D3D12Bridge::fail(std::string& why, const char* what, HRESULT hr) {
    char text[240];
    std::snprintf(text, sizeof(text), "%s (0x%08x)", what, static_cast<unsigned>(hr));
    why = failure = text;
    return false;
}

bool D3D12Bridge::open_game_device(ID3D11Device* device, ComPtr<IDXGIAdapter>& adapter, std::string& why) {
    HRESULT hr = device ? device->QueryInterface(IID_PPV_ARGS(&device11_)) : E_POINTER;
    if (FAILED(hr))
        return fail(why, "the game's Direct3D 11 device has no shared fences (Windows 10 1703 or newer needed)", hr);
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(hr = device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(hr = dxgi->GetAdapter(&adapter)))
        return fail(why, "the game's graphics adapter couldn't be found", hr);
    return true;
}

bool D3D12Bridge::create_device(IDXGIAdapter* adapter, std::string& why) {
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr))
        return fail(why, "no Direct3D 12 device on the game's graphics card", hr);
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HANDLE fenceHandle = nullptr;
    if (FAILED(hr = device_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_))) ||
        FAILED(hr = device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_))) ||
        FAILED(hr = device_->CreateSharedHandle(fence_.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle))) {
        reset();
        tried = true;
        return fail(why, "the Direct3D 12 queue and its fence couldn't be made", hr);
    }
    hr = device11_->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&fence11_));
    CloseHandle(fenceHandle);
    fenceHandle = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&releaseFence_))) &&
        SUCCEEDED(hr = device_->CreateSharedHandle(releaseFence_.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle))) {
        hr = device11_->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&releaseFence11_));
        CloseHandle(fenceHandle);
    }
    if (FAILED(hr)) {
        reset();
        tried = true;
        return fail(why, "the game's device couldn't open the shared fence", hr);
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue_->GetTimestampFrequency(&timestampFrequency_);
    return true;
}

bool D3D12Bridge::begin_list() {
    const UINT64 done = fence_->GetCompletedValue();
    Allocator* free = nullptr;
    for (Allocator& a : allocators_)
        if (a.busyUntil <= done) {
            free = &a;
            break;
        }
    if (!free) {
        Allocator a;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a.allocator))))
            return false;
        allocators_.push_back(a);
        free = &allocators_.back();
    }
    if (FAILED(free->allocator->Reset()))
        return false;
    free->busyUntil = UINT64_MAX;  // until end_list
    if (!list_)
        return SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, free->allocator.Get(), nullptr, IID_PPV_ARGS(&list_)));
    return SUCCEEDED(list_->Reset(free->allocator.Get(), nullptr));
}

UINT64 D3D12Bridge::end_list() {
    const bool closed = SUCCEEDED(list_->Close());
    UINT64 at = 0;
    if (closed) {
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        at = ++value_;
        queue_->Signal(fence_.Get(), at);
    }
    for (Allocator& a : allocators_)
        if (a.busyUntil == UINT64_MAX)
            a.busyUntil = at;
    return at;
}

bool D3D12Bridge::wait_for(UINT64 value, DWORD ms) {
    if (fence_->GetCompletedValue() >= value)
        return true;
    if (FAILED(fence_->SetEventOnCompletion(value, event_)))
        return false;
    return WaitForSingleObject(event_, ms) == WAIT_OBJECT_0;
}

void D3D12Bridge::barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, 0, from, to};
    list_->ResourceBarrier(1, &b);
}

// A texture both devices use: made here, opened on the game's device. `output`: the runtime writes it (D3D11 only reads
// it); else D3D11 draws it.
bool D3D12Bridge::shared(unsigned w, unsigned h, DXGI_FORMAT format, bool output, ComPtr<ID3D12Resource>& r12,
                         ComPtr<ID3D11Texture2D>& r11, HRESULT& hr) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w, d.Height = h, d.DepthOrArraySize = 1, d.MipLevels = 1, d.SampleDesc.Count = 1, d.Format = format;
    // A render target either way: the game's device opens no shared texture that is only the runtime's to write
    // (E_INVALIDARG, tests/nr_views_check.cpp).
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
              (output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    const char* step = "made on Direct3D 12";
    hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r12));
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr)) {
        step = "given a shared handle";
        hr = device_->CreateSharedHandle(r12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    }
    if (SUCCEEDED(hr)) {
        step = "opened on the game's device";
        hr = device11_->OpenSharedResource1(handle, IID_PPV_ARGS(&r11));
    }
    if (handle)
        CloseHandle(handle);
    if (FAILED(hr))
        simqvfr::logf("%s: a shared %ux%u texture (format %d, %s) couldn't be %s (0x%08x)", tag_, w, h, static_cast<int>(format),
                      output ? outputName_ : "an input", step, static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}

bool D3D12Bridge::make_view(BridgeView* v, unsigned w, unsigned h, DXGI_FORMAT colour, unsigned outW, unsigned outH,
                            DXGI_FORMAT output, HRESULT& hr) {
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2;
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 2 * sizeof(UINT64), bd.Height = 1, bd.DepthOrArraySize = 1, bd.MipLevels = 1, bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return shared(w, h, colour, false, v->colour, v->colour11, hr) &&
           shared(w, h, DXGI_FORMAT_R32_FLOAT, false, v->depth, v->depth11, hr) &&
           shared(w, h, DXGI_FORMAT_R16G16_FLOAT, false, v->vectors, v->vectors11, hr) &&
           shared(outW, outH, output, true, v->output, v->output11, hr) &&
           SUCCEEDED(hr = device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&v->queries))) &&
           SUCCEEDED(hr = device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           nullptr, IID_PPV_ARGS(&v->times)));
}

bool D3D12Bridge::begin_evaluation(ID3D11DeviceContext* context, BridgeView* v, size_t backlog, unsigned behind,
                                   unsigned& result, Evaluating& e) {
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(&e.context4));
    if (FAILED(hr) || FAILED(hr = device_->GetDeviceRemovedReason())) {
        result = static_cast<unsigned>(hr);
        return false;
    }
    collect_time(v);
    // The GPU's backlog of our evaluations: with `backlog` of them still to do, this one isn't added to the pile.
    const UINT64 reached = fence_->GetCompletedValue();
    while (!outstanding_.empty() && outstanding_.front() <= reached)
        outstanding_.pop_front();
    if (outstanding_.size() >= backlog) {
        result = behind;
        return false;
    }
    if (!begin_list())
        return false;
    // D3D11's draws and copies into the inputs first: the queue waits for this value before the list below runs.
    const UINT64 inputs = ++value_;
    e.context4->Signal(fence11_.Get(), inputs);
    context->Flush();
    queue_->Wait(fence_.Get(), inputs);

    ID3D12Resource* const inputsOf[] = {v->colour.Get(), v->depth.Get(), v->vectors.Get()};
    for (ID3D12Resource* r : inputsOf)
        barrier(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(v->output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    e.timed = !v->timedAt;  // the last timed one has been read
    if (e.timed)
        list_->EndQuery(v->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    return true;
}

bool D3D12Bridge::end_evaluation(BridgeView* v, Evaluating& e) {
    if (e.timed) {
        list_->EndQuery(v->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list_->ResolveQueryData(v->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, v->times.Get(), 0);
    }
    ID3D12Resource* const inputsOf[] = {v->colour.Get(), v->depth.Get(), v->vectors.Get()};
    for (ID3D12Resource* r : inputsOf)
        barrier(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(v->output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    const UINT64 done = end_list();
    if (!done)
        return false;
    if (e.timed)
        v->timedAt = done;
    v->lastSubmit = done;
    outstanding_.push_back(done);
    e.context4->Wait(fence11_.Get(), done);
    return true;
}

void D3D12Bridge::park(BridgeView* v) {
    v->releaseValue = 0;  // the game's device never touched it
    v->releasedAt = collects_;
    released_.push_back(v);
}

void D3D12Bridge::discard(BridgeView* v) {
    if (queue_ && fence_ && v->lastSubmit && fence_->GetCompletedValue() < v->lastSubmit) {
        park(v);
        return;
    }
    destroy_(v);
}

void D3D12Bridge::release(ID3D11DeviceContext* context, BridgeView* v, IUnknown* const* keep, unsigned count) {
    if (!v)
        return;
    for (unsigned i = 0; keep && i < count; ++i)
        if (keep[i])
            v->keep.emplace_back(keep[i]);
    // The game's device signals once it has run everything queued so far: every draw into the view's inputs, every
    // read of its output.
    ComPtr<ID3D11DeviceContext4> context4;
    if (context && releaseFence11_ && !device_lost() && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
        v->releaseValue = ++releaseValue_;
        context4->Signal(releaseFence11_.Get(), v->releaseValue);
    } else {
        v->releaseValue = UINT64_MAX;  // never known to be through: left to shutdown()
    }
    v->releasedAt = collects_;
    released_.push_back(v);
}

void D3D12Bridge::destroy_finished() {
    if (released_.empty() || !fence_ || !releaseFence_)
        return;
    const UINT64 done = fence_->GetCompletedValue(), let = releaseFence_->GetCompletedValue();
    if (done == UINT64_MAX || let == UINT64_MAX)  // a lost device: nothing is known to be through
        return;
    for (auto it = released_.begin(); it != released_.end();) {
        BridgeView* v = *it;
        if (done >= v->lastSubmit && let >= v->releaseValue && collects_ >= v->releasedAt + kReleaseFrames) {
            destroy_(v);
            it = released_.erase(it);
        } else {
            ++it;
        }
    }
}

// The last timed submission's timestamps into the view's sums, once it is through.
void D3D12Bridge::collect_time(BridgeView* v) {
    if (!v->timedAt || !timestampFrequency_ || fence_->GetCompletedValue() < v->timedAt)
        return;
    v->timedAt = 0;
    UINT64* stamps = nullptr;
    const D3D12_RANGE read{0, 2 * sizeof(UINT64)};
    if (FAILED(v->times->Map(0, &read, reinterpret_cast<void**>(&stamps))) || !stamps)
        return;
    if (stamps[1] > stamps[0]) {
        v->msSum += 1000.0 * static_cast<double>(stamps[1] - stamps[0]) / static_cast<double>(timestampFrequency_);
        ++v->msCount;
    }
    const D3D12_RANGE none{0, 0};
    v->times->Unmap(0, &none);
}

double D3D12Bridge::take_gpu_ms(BridgeView* v) {
    if (!v)
        return 0;
    collect_time(v);
    const double ms = v->msCount ? v->msSum / v->msCount : 0;
    v->msSum = 0;
    v->msCount = 0;
    return ms;
}

bool D3D12Bridge::shutdown(ID3D11DeviceContext* context, const char* work, const char* leftNote) {
    // Both devices through all that is queued: the game's (flushed here; its signal on the release fence comes after
    // everything before it) and ours.
    // A lost device has nothing queued that could still run: everything of it is released in full (a Direct3D 12
    // device is one object a process and adapter while anything holds it, so one reference left would hand the lost
    // device back to the next start()).
    const bool lost = device_lost();
    bool idle = true;
    if (!lost && queue_ && fence_ && event_) {
        ComPtr<ID3D11DeviceContext4> context4;
        UINT64 let = 0;
        if (context && releaseFence11_ && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            let = ++releaseValue_;
            context4->Signal(releaseFence11_.Get(), let);
            context->Flush();
        } else if (!released_.empty()) {
            idle = false;
        }
        const UINT64 at = ++value_;
        idle = idle && SUCCEEDED(queue_->Signal(fence_.Get(), at)) && wait_for(at, kShutdownWaitMs);
        if (idle && let) {
            idle = SUCCEEDED(releaseFence_->SetEventOnCompletion(let, event_)) &&
                   WaitForSingleObject(event_, kShutdownWaitMs) == WAIT_OBJECT_0;
            idle = idle && releaseFence_->GetCompletedValue() != UINT64_MAX;
        }
        idle = idle && fence_->GetCompletedValue() != UINT64_MAX;
    }
    if (lost)
        simqvfr::logf("%s: its Direct3D 12 device is lost: everything of it released (%u released views among it)", tag_,
                      static_cast<unsigned>(released_.size()));
    if (!idle) {
        // Work of ours still queued on a live device: nothing is destroyed. Left to the process's end.
        simqvfr::logf("%s: the GPU didn't finish %s's work within 3 s: its Direct3D 12 objects (%u released views among them) "
                      "are left as they are, not destroyed%s",
                      tag_, work, static_cast<unsigned>(released_.size()), leftNote);
        (void)device_.Detach();
        (void)device11_.Detach();
        (void)queue_.Detach();
        (void)list_.Detach();
        (void)fence_.Detach();
        (void)fence11_.Detach();
        (void)releaseFence_.Detach();
        (void)releaseFence11_.Detach();
        for (Allocator& a : allocators_)
            (void)a.allocator.Detach();
        for (BridgeView* v : released_)
            for (ComPtr<IUnknown>& k : v->keep)
                (void)k.Detach();
        clear();
        return false;
    }
    for (BridgeView* v : released_)
        destroy_(v);
    released_.clear();
    return true;
}

void D3D12Bridge::reset() {
    if (event_)
        CloseHandle(event_);
    clear();
}

void D3D12Bridge::clear() {
    started = tried = false;
    failure.clear();
    device11_.Reset();
    device_.Reset();
    queue_.Reset();
    list_.Reset();
    allocators_.clear();
    fence_.Reset();
    fence11_.Reset();
    value_ = 0;
    releaseFence_.Reset();
    releaseFence11_.Reset();
    releaseValue_ = 0;
    event_ = nullptr;
    timestampFrequency_ = 0;
    released_.clear();
    collects_ = 0;
    outstanding_.clear();
}

}  // namespace simqvfr
