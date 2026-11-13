#include "probe/d3d11_compose_timer.h"

#include <array>
#include <mutex>
#include <d3d11.h>
#include <wrl/client.h>
#include "log.h"

namespace simqvfr {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kSlots = 6;  // spans in flight: a few frames of GPU latency

struct Slot {
    ComPtr<ID3D11Query> disjoint, begin, end;
    bool pending = false;  // ended, not read yet
};

struct Timer {
    std::mutex mutex;
    ComPtr<ID3D11DeviceContext> context;
    std::array<Slot, kSlots> slots;
    int open = -1;  // the slot begun and not ended
    int next = 0;
    double totalMs = 0;
    int frames = 0;
} timer;

// Reads every finished span into the totals (under the mutex).
void read_finished() {
    for (Slot& s : timer.slots) {
        if (!s.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (timer.context->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            timer.context->GetData(s.begin.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            timer.context->GetData(s.end.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;  // not yet
        s.pending = false;
        if (!dj.Disjoint && dj.Frequency && t1 >= t0) {
            timer.totalMs += static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
            ++timer.frames;
        }
    }
}

}  // namespace

bool d3d11_compose_timer_start(ID3D11Device* device) {
    std::lock_guard guard(timer.mutex);
    if (timer.context)
        return true;
    if (!device)
        return false;
    const D3D11_QUERY_DESC dq = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, tq = {D3D11_QUERY_TIMESTAMP, 0};
    for (Slot& s : timer.slots)
        if (FAILED(device->CreateQuery(&dq, &s.disjoint)) || FAILED(device->CreateQuery(&tq, &s.begin)) ||
            FAILED(device->CreateQuery(&tq, &s.end))) {
            timer.slots = {};
            log_line("d3d11 compose timer: queries couldn't be made; no layer GPU time");
            return false;
        }
    device->GetImmediateContext(&timer.context);
    timer.open = -1;
    timer.next = 0;
    timer.totalMs = 0;
    timer.frames = 0;
    log_line("d3d11 compose timer: timing the layer's composition on the GPU");
    return true;
}

void d3d11_compose_timer_stop() {
    std::lock_guard guard(timer.mutex);
    timer.slots = {};
    timer.context.Reset();
    timer.open = -1;
    timer.totalMs = 0;
    timer.frames = 0;
}

void d3d11_compose_timer_begin() {
    std::lock_guard guard(timer.mutex);
    if (!timer.context || timer.open >= 0)
        return;
    read_finished();
    for (int i = 0; i < kSlots; ++i) {
        const int at = (timer.next + i) % kSlots;
        Slot& s = timer.slots[at];
        if (s.pending)
            continue;
        timer.context->Begin(s.disjoint.Get());
        timer.context->End(s.begin.Get());
        timer.open = at;
        timer.next = (at + 1) % kSlots;
        return;
    }
    // every slot still in flight: this frame isn't timed
}

void d3d11_compose_timer_end() {
    std::lock_guard guard(timer.mutex);
    if (!timer.context || timer.open < 0)
        return;
    Slot& s = timer.slots[timer.open];
    timer.context->End(s.end.Get());
    timer.context->End(s.disjoint.Get());
    s.pending = true;
    timer.open = -1;
}

double d3d11_compose_timer_collect(int& frames) {
    std::lock_guard guard(timer.mutex);
    frames = timer.frames;
    const double ms = timer.frames ? timer.totalMs / timer.frames : -1;
    timer.totalMs = 0;
    timer.frames = 0;
    return ms;
}

}  // namespace simqvfr
