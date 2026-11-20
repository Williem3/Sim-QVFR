// Where the eyes look, how far (the API path): the left focus view's own depth at its centre (the left eye's box centre,
// as this frame's focus passes aimed it), read once a frame when the focus tile culling runs (the depth is then a shader
// resource: focus_tiles_record), so the layer converges both eyes' focus boxes there (GameAdapter::qvfr_fixation_m,
// quad/focus_converge.h), as LMU's and ACC's. Nine texels round the centre are copied by a one-thread compute pass into
// a small buffer, then into a readback ring read kAge frames later: nothing waits on the GPU. The depth's terms come
// from the focus view's inverse projection (the tile culling's), never assumed. [experiment] replay_fixation=0: no reads.
#include "replay_internal.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../engine/dlss_math.h"
#include "../../log.h"
#include "../../quad/focus_converge.h"
#include "replay_probe.h"

namespace simqvfr {

bool replay_fixation_terms(const float (&inverse)[16], double& a, double& b, double& tx, double& ty) {
    double q[4][4], p[4][4];
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            q[i][k] = inverse[k * 4 + i];
    if (!replay_detail::invert(q, p))
        return false;
    // clip w = p32 z (+ p33, 0 for a perspective view), depth = clip z / clip w = p22 / p32 + p23 / w.
    if (!(std::fabs(p[3][2]) > 1e-9) || std::fabs(p[3][3]) > 1e-6 * std::fabs(p[3][2]))
        return false;
    const double ta = p[2][2] / p[3][2], tb = p[2][3];
    double nearM = 0, farM = 0;
    bool infinite = false;
    if (!std::isfinite(ta) || !std::isfinite(tb) || !dlss::near_far(ta, tb, nearM, farM, infinite) || nearM > 5)
        return false;
    // The view's centre ray: the point at the NDC centre (any depth), as tangents (sign aside: the ray's length only).
    const double c[4] = {0, 0, 0.5, 1};
    double v[3] = {};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 4; ++k)
            v[i] += q[i][k] * c[k];
    if (!(std::fabs(v[2]) > 1e-12))
        return false;
    a = ta;
    b = tb;
    tx = v[0] / v[2];
    ty = v[1] / v[2];
    return true;
}

namespace replay_detail {
namespace {

constexpr char kReadShader[] = R"(
Texture2D<float> depth : register(t0);
cbuffer Params : register(b0) { int px; int py; int step; int w; int h; uint seq; };
RWByteAddressBuffer dst : register(u0);
[numthreads(1, 1, 1)] void main() {
    for (int i = 0; i < 9; ++i) {
        const int x = clamp(px + (i % 3 - 1) * step, 0, w - 1), y = clamp(py + (i / 3 - 1) * step, 0, h - 1);
        dst.Store(i * 4, asuint(depth.Load(int3(x, y, 0))));
    }
    dst.Store(36, seq);
}
)";

constexpr int kRing = 4;
constexpr UINT kSlotBytes = 64;
constexpr uint64_t kAge = 3;  // frames before a slot is read: the GPU is done with it by then

struct Slot {
    uint64_t frame = UINT64_MAX;  // when queued; UINT64_MAX: free
    uint32_t seq = 0;
    double a = 0, b = 0, tx = 0, ty = 0;
};

struct Fixation {
    SRWLOCK lock = SRWLOCK_INIT;
    bool tried = false, ready = false;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> values;    // default heap, UAV: kRing slots
    ComPtr<ID3D12Resource> readback;  // readback heap: kRing slots, mapped for good
    const uint8_t* mapped = nullptr;
    Slot slots[kRing];
    uint32_t nextSeq = 1;
    uint64_t lastQueuedFrame = UINT64_MAX;
    FixationFilter filter;
    std::atomic<double> metres{0};
    // the 900-frame line
    uint64_t queued = 0, reads = 0, sky = 0, stale = 0;
    double minM = 1e9, maxM = 0;
    int termsSaid = 0;
    double lastA = 0, lastB = 0;
    int pixel[2] = {};
} fx;

bool init() {
    fx.tried = true;
    ComPtr<ID3DBlob> code, errors, blob;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 6};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 0};
    const D3D12_ROOT_SIGNATURE_DESC desc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if (FAILED(D3DCompile(kReadShader, sizeof(kReadShader) - 1, "focus-fixation", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                          &code, &errors)) ||
        FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&fx.root)))) {
        logf("replay fixation: the depth read shader failed (%s): the focus boxes stay parallel",
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = fx.root.Get();
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    const D3D12_HEAP_PROPERTIES defaultHeap{D3D12_HEAP_TYPE_DEFAULT}, readHeap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kRing} * kSlotBytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                               {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    bool ok = SUCCEEDED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&fx.pso))) &&
              SUCCEEDED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                        IID_PPV_ARGS(&fx.values)));
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    ok = ok && SUCCEEDED(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&fx.readback)));
    void* data = nullptr;
    const D3D12_RANGE all{0, size_t{kRing} * kSlotBytes};
    ok = ok && SUCCEEDED(fx.readback->Map(0, &all, &data)) && data;
    if (!ok) {
        log_line("replay fixation: no buffers for the depth read: the focus boxes stay parallel");
        fx.pso.Reset();
        fx.values.Reset();
        fx.readback.Reset();
        return false;
    }
    fx.mapped = static_cast<const uint8_t*>(data);
    log_line("replay fixation: on (the left focus view's depth at its centre converges the focus boxes)");
    return true;
}

}  // namespace

void focus_fixation_record(List* l, D3D12_GPU_DESCRIPTOR_HANDLE depthSrv, UINT fw, UINT fh, const float (&inverse)[16]) {
    if (!focus.settings.fixation || !l || !fw || !fh)
        return;
    const uint64_t frame = focus.frame.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&fx.lock);
    if (!fx.tried)
        fx.ready = init();
    double a = 0, b = 0, tx = 0, ty = 0;
    Slot* slot = nullptr;
    if (fx.ready && fx.lastQueuedFrame != frame && replay_fixation_terms(inverse, a, b, tx, ty))
        for (Slot& s : fx.slots)
            if (s.frame == UINT64_MAX) {
                slot = &s;
                break;
            }
    if (!slot) {
        ReleaseSRWLockExclusive(&fx.lock);
        return;
    }
    const int index = static_cast<int>(slot - fx.slots);
    slot->frame = frame;
    slot->seq = fx.nextSeq++;
    slot->a = a;
    slot->b = b;
    slot->tx = tx;
    slot->ty = ty;
    fx.lastQueuedFrame = frame;
    ++fx.queued;
    if ((std::fabs(a - fx.lastA) > 1e-6 * (1 + std::fabs(a)) || std::fabs(b - fx.lastB) > 1e-6 * (1 + std::fabs(b))) &&
        fx.termsSaid < 4) {
        ++fx.termsSaid;
        double nearM = 0, farM = 0;
        bool infinite = false;
        dlss::near_far(a, b, nearM, farM, infinite);
        logf("replay fixation: the focus depth runs %.3f m to %s%.0f m (%s; depth = %.6f + %.6f / w)", nearM,
             infinite ? "past " : "", farM, b > 0 ? "reversed" : "0 near", a, b);
    }
    fx.lastA = a;
    fx.lastB = b;
    // The left focus view: the depth's left half, its centre the box centre.
    const int32_t params[6] = {static_cast<int32_t>(fw / 2), static_cast<int32_t>(fh / 2),
                               static_cast<int32_t>(std::max(2u, fw / 150)), static_cast<int32_t>(fw),
                               static_cast<int32_t>(fh), static_cast<int32_t>(slot->seq)};
    fx.pixel[0] = params[0];
    fx.pixel[1] = params[1];
    ReleaseSRWLockExclusive(&fx.lock);
    const UINT64 at = UINT64{static_cast<UINT>(index)} * kSlotBytes;
    l->SetComputeRootSignature(fx.root.Get());
    l->SetPipelineState(fx.pso.Get());
    l->SetComputeRootDescriptorTable(0, depthSrv);
    l->SetComputeRoot32BitConstants(1, 6, params, 0);
    l->SetComputeRootUnorderedAccessView(2, fx.values->GetGPUVirtualAddress() + at);
    l->Dispatch(1, 1, 1);
    barrier(l, fx.values.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyBufferRegion(fx.readback.Get(), at, fx.values.Get(), at, kSlotBytes);
    barrier(l, fx.values.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void focus_fixation_frame_end(uint64_t ended) {
    AcquireSRWLockExclusive(&fx.lock);
    if (fx.ready) {
        // The oldest slot old enough: its values, if the copy that carries its number has landed.
        Slot* oldest = nullptr;
        for (Slot& s : fx.slots)
            if (s.frame != UINT64_MAX && ended >= s.frame + kAge && (!oldest || s.frame < oldest->frame))
                oldest = &s;
        if (oldest) {
            const uint8_t* p = fx.mapped + (oldest - fx.slots) * kSlotBytes;
            uint32_t seq = 0;
            std::memcpy(&seq, p + 36, 4);
            if (seq == oldest->seq) {
                float values[9];
                std::memcpy(values, p, sizeof(values));
                int skyCount = 0;
                const double median = fixation_from_depths(values, 9, oldest->tx, oldest->ty, oldest->a, oldest->b, skyCount);
                const double published = fx.filter.update(median, 1.0 / 90);
                fx.metres.store(published, std::memory_order_relaxed);
                ++fx.reads;
                fx.sky += skyCount > 4 ? 1 : 0;
                fx.minM = std::min(fx.minM, published);
                fx.maxM = std::max(fx.maxM, published);
            } else {
                ++fx.stale;
            }
            oldest->frame = UINT64_MAX;  // free again either way
        }
        // A slot the GPU never filled (its list not executed: a dropped frame): freed after a while.
        for (Slot& s : fx.slots)
            if (s.frame != UINT64_MAX && ended > s.frame + 30)
                s.frame = UINT64_MAX;
    }
    if (fx.ready && ended % 900 == 0 && ended) {
        logf("replay fixation: %llu reads of %llu queued (%llu not landed), %llu at the sky over the last 900 frames; "
             "distance now %.2f m (%.2f-%.2f); the left focus view's centre at texel %d,%d",
             static_cast<unsigned long long>(fx.reads), static_cast<unsigned long long>(fx.queued),
             static_cast<unsigned long long>(fx.stale), static_cast<unsigned long long>(fx.sky), fx.metres.load(),
             fx.minM < 1e9 ? fx.minM : 0.0, fx.maxM, fx.pixel[0], fx.pixel[1]);
        fx.reads = fx.queued = fx.sky = fx.stale = 0;
        fx.minM = 1e9;
        fx.maxM = 0;
    }
    ReleaseSRWLockExclusive(&fx.lock);
}

double focus_fixation_m() { return fx.metres.load(std::memory_order_relaxed); }

}  // namespace replay_detail

double replay_probe_fixation_m() { return replay_detail::focus_fixation_m(); }

}  // namespace simqvfr
