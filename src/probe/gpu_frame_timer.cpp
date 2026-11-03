#include "gpu_frame_timer.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "../log.h"

namespace aceqv {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kSlots = 16;  // frames in flight that can be timed
constexpr int kMarks = 3;   // GpuMark values
constexpr size_t kWindowMax = 20000;  // samples a window keeps when nobody collects it (the overlay off)

// Samples since a reader last collected them: 0 the performance overlay's, 1 the log's, 2 the QVFR optimizer's.
struct Window {
    std::vector<float> render, interval, compose;
    uint64_t skipped = 0;
    void add(std::vector<float> Window::*samples, float v) {
        auto& s = this->*samples;
        if (s.size() < kWindowMax)
            s.push_back(v);
    }
    void clear() {
        render.clear();
        interval.clear();
        compose.clear();
        skipped = 0;
    }
};

struct Slot {
    ComPtr<ID3D12CommandAllocator> allocator[kMarks];
    ComPtr<ID3D12GraphicsCommandList> list[kMarks];
    uint64_t frame = 0;
    uint64_t lastSignal = 0;  // fence value after this slot's last timestamp
    bool used[kMarks] = {};
    bool harvested = true;
};

struct Timer {
    std::mutex mutex;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Fence> fence;
    const uint64_t* results = nullptr;  // the readback buffer, mapped
    uint64_t fenceValue = 0;
    double msPerTick = 0;
    Slot slots[kSlots];
    uint64_t frame = 0;
    int current = -1;  // the slot of the frame being marked, -1 when it is not timed
    uint64_t lastRenderedFrame = 0, lastRenderedTicks = 0;
    Window windows[3];
    bool started = false;
};
Timer timer;

// Under the mutex: reads every finished slot's timestamps into the samples.
void harvest() {
    const uint64_t done = timer.fence->GetCompletedValue();
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = timer.slots[i];
        if (s.harvested || i == timer.current || s.lastSignal > done)
            continue;
        s.harvested = true;
        const uint64_t* t = timer.results + i * kMarks;
        const auto begin = static_cast<int>(GpuMark::kBegin), rendered = static_cast<int>(GpuMark::kRendered),
                   composed = static_cast<int>(GpuMark::kComposed);
        if (!s.used[begin] || !s.used[rendered] || t[rendered] < t[begin])
            continue;
        for (Window& w : timer.windows) {
            w.add(&Window::render, static_cast<float>((t[rendered] - t[begin]) * timer.msPerTick));
            if (s.used[composed] && t[composed] >= t[rendered])
                w.add(&Window::compose, static_cast<float>((t[composed] - t[rendered]) * timer.msPerTick));
            if (timer.lastRenderedFrame + 1 == s.frame && t[rendered] > timer.lastRenderedTicks)
                w.add(&Window::interval, static_cast<float>((t[rendered] - timer.lastRenderedTicks) * timer.msPerTick));
        }
        timer.lastRenderedFrame = s.frame;
        timer.lastRenderedTicks = t[rendered];
    }
}

// Under the mutex: one timestamp into the queue, resolved into the readback buffer.
void stamp(Slot& s, int slot, int mark) {
    const UINT index = static_cast<UINT>(slot * kMarks + mark);
    if (FAILED(s.allocator[mark]->Reset()) || FAILED(s.list[mark]->Reset(s.allocator[mark].Get(), nullptr)))
        return;
    s.list[mark]->EndQuery(timer.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    s.list[mark]->ResolveQueryData(timer.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index, 1, timer.readback.Get(),
                                   index * sizeof(uint64_t));
    if (FAILED(s.list[mark]->Close()))
        return;
    ID3D12CommandList* lists[] = {s.list[mark].Get()};
    timer.queue->ExecuteCommandLists(1, lists);
    timer.queue->Signal(timer.fence.Get(), ++timer.fenceValue);
    s.lastSignal = timer.fenceValue;
    s.used[mark] = true;
}

double percentile(std::vector<float>& v, double p) {
    if (v.empty())
        return 0;
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

double average(const std::vector<float>& v) {
    double sum = 0;
    for (const float x : v)
        sum += x;
    return v.empty() ? 0 : sum / static_cast<double>(v.size());
}

}  // namespace

bool gpu_frame_timer_start(ID3D12Device* device, ID3D12CommandQueue* queue) {
    std::lock_guard guard(timer.mutex);
    if (timer.started || !device || !queue)
        return timer.started;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        log_line("views4 gpu: the session queue is not a direct queue; GPU frame timing off");
        return false;
    }
    UINT64 frequency = 0;
    D3D12_QUERY_HEAP_DESC heapDesc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * kMarks, 0};
    D3D12_HEAP_PROPERTIES heapProps{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = kSlots * kMarks * sizeof(uint64_t);
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency &&
              SUCCEEDED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&timer.heap))) &&
              SUCCEEDED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &buffer,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&timer.readback))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timer.fence)));
    for (Slot& s : timer.slots)
        for (int m = 0; ok && m < kMarks; ++m) {
            ok = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator[m]))) &&
                 SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator[m].Get(), nullptr,
                                                     IID_PPV_ARGS(&s.list[m])));
            if (ok)
                s.list[m]->Close();
        }
    void* mapped = nullptr;
    D3D12_RANGE whole{0, static_cast<SIZE_T>(buffer.Width)};
    ok = ok && SUCCEEDED(timer.readback->Map(0, &whole, &mapped));
    if (!ok) {
        log_line("views4 gpu: could not create the timestamp queries; GPU frame timing off");
        timer.heap.Reset();
        timer.readback.Reset();
        timer.fence.Reset();
        for (Slot& s : timer.slots)
            for (int m = 0; m < kMarks; ++m) {
                s.list[m].Reset();
                s.allocator[m].Reset();
            }
        return false;
    }
    timer.results = static_cast<const uint64_t*>(mapped);
    timer.queue = queue;
    timer.msPerTick = 1000.0 / static_cast<double>(frequency);
    timer.started = true;
    log_line("views4 gpu: GPU frame timing on (timestamps on the session's queue)");
    return true;
}

void gpu_frame_timer_stop() {
    std::lock_guard guard(timer.mutex);
    if (!timer.started)
        return;
    // Let in-flight timestamps finish before their allocators and buffer go.
    if (timer.fence->GetCompletedValue() < timer.fenceValue) {
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (done && SUCCEEDED(timer.fence->SetEventOnCompletion(timer.fenceValue, done)))
            WaitForSingleObject(done, 1000);
        if (done)
            CloseHandle(done);
    }
    timer.readback->Unmap(0, nullptr);
    timer.results = nullptr;
    for (Slot& s : timer.slots) {
        for (int m = 0; m < kMarks; ++m) {
            s.list[m].Reset();
            s.allocator[m].Reset();
        }
        s = Slot{};
    }
    timer.heap.Reset();
    timer.readback.Reset();
    timer.fence.Reset();
    timer.queue.Reset();
    timer.current = -1;
    for (Window& w : timer.windows)
        w.clear();
    timer.started = false;
}

void gpu_frame_timer_mark(GpuMark mark) {
    std::lock_guard guard(timer.mutex);
    if (!timer.started)
        return;
    const int m = static_cast<int>(mark);
    if (mark == GpuMark::kBegin) {
        timer.current = -1;
        harvest();
        const int slot = static_cast<int>(++timer.frame % kSlots);
        Slot& s = timer.slots[slot];
        if (!s.harvested) {  // still in flight sixteen frames later: leave this frame out
            for (Window& w : timer.windows)
                ++w.skipped;
            return;
        }
        std::memset(s.used, 0, sizeof(s.used));
        s.frame = timer.frame;
        s.harvested = false;
        timer.current = slot;
    }
    if (timer.current < 0)
        return;
    Slot& s = timer.slots[timer.current];
    if (!s.used[m])
        stamp(s, timer.current, m);
}

namespace {

// The window's frames, and resets it.
GpuFrameStats collect(Window& w) {
    std::lock_guard guard(timer.mutex);
    GpuFrameStats stats;
    if (!timer.started)
        return stats;
    harvest();
    stats.frames = w.render.size();
    stats.renderAvg = average(w.render);
    stats.renderP50 = percentile(w.render, 0.5);
    stats.renderP90 = percentile(w.render, 0.9);
    stats.intervalP50 = percentile(w.interval, 0.5);
    stats.intervalP90 = percentile(w.interval, 0.9);
    stats.composeAvg = average(w.compose);
    stats.composeP90 = percentile(w.compose, 0.9);
    stats.skipped = w.skipped;
    w.clear();
    return stats;
}

}  // namespace

GpuFrameStats gpu_frame_timer_collect() { return collect(timer.windows[0]); }

GpuFrameStats gpu_frame_timer_collect_optimizer() { return collect(timer.windows[2]); }

void gpu_frame_timer_report() {
    if (!timer.started)
        return;
    const GpuFrameStats s = collect(timer.windows[1]);
    logf("views4 gpu: ACE's frame on its queue (xrBeginFrame to eye-image release) p50 %.2f ms, p90 %.2f, avg %.2f over "
         "%zu frames; rendered-to-rendered p50 %.2f ms, p90 %.2f; composition (with the helmet layer) %.3f ms avg, "
         "p90 %.3f; frames not timed %llu",
         s.renderP50, s.renderP90, s.renderAvg, s.frames, s.intervalP50, s.intervalP90, s.composeAvg, s.composeP90,
         static_cast<unsigned long long>(s.skipped));
}

}  // namespace aceqv
