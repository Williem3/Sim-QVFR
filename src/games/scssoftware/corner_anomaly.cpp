// The violet-flash anomaly log (dev builds, [experiment] scs_violet_log=1). ATS and ETS2 show, rarely (about one frame
// in a hundred) and in one eye, ground or white UI tinted by the corner's pair values (0.4, 0.4, 2.5): our copies of the
// game's large constant buffers (corner_mirrors.cpp) rewrote a white (1,1,1,1) that a shader reads as a colour as if it
// were normalized_to_texture. The sampling diagnostic (corner_mirror_diag.cpp, one frame in 90) never caught the false
// match. This one watches every frame without reading the buffers back:
//   - the mirror pass (kCornerMirrorCs with its tag) appends each pair it rewrites to a log buffer on the GPU: which
//     mirror and copy, the vector's index, and the four vectors around it (the one before, the (1,1,1,1), the size, the
//     one after);
//   - while the eyes are drawn, each draw's vertex and pixel constant buffer 0 (the only one Prism's shaders read) is
//     noted with its range and shaders, if it is one of our copies;
//   - three frames on (the log read without waiting) each rewritten pair is set against the draws that read it and the
//     rows their shader reads by a fixed index (mvr_probe's kCbRows4Guid): a "kind" is (stage, shader, the pair's row in
//     the range, which of the two rows it reads). Kinds are counted per frame over the session.
// After a warm-up a kind never seen before is logged at once in full (its frame, mirror, index, vectors, draw, shader,
// target); every 900 frames the rarest kinds are listed; Ctrl+Alt+M (the player saw a flash) lists the new kinds of the
// last three seconds and the rarest. Log only: the pairs are rewritten as before.
#include "corner_internal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <tuple>
#include <vector>

#include "../../dev_build.h"
#include "../../engine/multi_view_shaders.h"
#include "../../log.h"

namespace scs {
namespace detail {
namespace {

constexpr UINT kCapacity = 4096;          // rewritten pairs logged a frame (more are counted, not kept)
constexpr UINT kEntryBytes = 80;          // tag, index, two words of padding, then four vectors
constexpr UINT kHeaderBytes = 16;         // the count (and padding)
constexpr int kSlots = 6;                 // log buffers in flight
constexpr uint64_t kReadAfter = 3;        // frames before a log is read (without waiting)
constexpr uint64_t kWarmup = 600;         // frames read before a new kind counts as an anomaly
constexpr uint64_t kSummaryEvery = 900;
constexpr uint64_t kMarkWindow = 216;     // frames a mark looks back over (three seconds at 72 Hz)
constexpr int kMaxReports = 120;

struct DrawRec {
    uint32_t draw = 0;          // the frame's draw number (draws with a copy bound)
    int mirror = -1;            // Mirror::id of the copy bound
    uint32_t serial = 0;        // its serial (24 bits) when drawn
    UINT first = 0, count = 0;  // the bound range, in vectors
    uint8_t stage = 0;          // 0 vertex, 1 pixel
    uint64_t shader = 0;        // kShaderIdGuid
    UINT w = 0, h = 0;          // the target
};

struct Slot {
    ComPtr<ID3D11Buffer> log, staging;
    ComPtr<ID3D11UnorderedAccessView> view;
    uint64_t frame = 0;
    bool used = false;      // a mirror pass wrote to it this frame
    bool inFlight = false;  // copied for reading, not read yet
    std::vector<DrawRec> draws;
};

struct Kind {
    uint8_t stage;
    uint64_t shader;
    UINT row;      // the (1,1,1,1)'s row in the bound range
    uint8_t reads;  // 1 the (1,1,1,1) row, 2 the size row, 3 both; 0: nobody reads either
    bool operator<(const Kind& o) const {
        return std::tie(stage, shader, row, reads) < std::tie(o.stage, o.shader, o.row, o.reads);
    }
};
struct KindStat {
    uint64_t frames = 0, firstFrame = 0, lastFrame = 0, countedFrame = ~0ull;
    float around[4][4] = {};
    uint32_t index = 0, draw = 0;
    UINT w = 0, h = 0;
    int mirror = -1;
};

using Rows = std::array<uint64_t, 4 * 64>;

struct Log {
    bool on = false, ready = false, failed = false;
    Slot slots[kSlots];
    int current = 0;
    uint64_t processedFrames = 0, entries = 0, overflowFrames = 0, maxEntries = 0, unread = 0, reports = 0, lost = 0;
    uint64_t lastSummary = 0;
    uint32_t drawNo = 0;
    std::map<Kind, KindStat> kinds;
    std::map<uint64_t, Rows> rows;
    std::deque<std::pair<uint64_t, Kind>> recentNew;  // kinds first seen after the warm-up, with their frame
    bool markDown = false;
} a;

void rows_of(ID3D11DeviceChild* shader, uint64_t& id) {
    id = 0;
    UINT size = sizeof(id);
    if (!shader || FAILED(shader->GetPrivateData(simqvfr::mvs::kShaderIdGuid, &size, &id)))
        return;
    if (a.rows.count(id))
        return;
    Rows r{};
    UINT rs = sizeof(r);
    if (FAILED(shader->GetPrivateData(simqvfr::mvs::kCbRows4Guid, &rs, r.data())) || rs != sizeof(r))
        r.fill(0);
    a.rows.emplace(id, r);
}

bool reads_row(uint64_t shader, UINT row) {  // slot 0's rows (kCbRows4Guid: 64 words a slot)
    const auto it = a.rows.find(shader);
    return it != a.rows.end() && row < 4096 && ((it->second[row / 64] >> (row % 64)) & 1) != 0;
}

bool make_slots() {
    const D3D11_BUFFER_DESC ld = {kHeaderBytes + kCapacity * kEntryBytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                  D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    const D3D11_BUFFER_DESC sd = {ld.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer = {0, ld.ByteWidth / 4, D3D11_BUFFER_UAV_FLAG_RAW};
    for (Slot& s : a.slots)
        if (FAILED(g.device->CreateBuffer(&ld, nullptr, &s.log)) || FAILED(g.device->CreateBuffer(&sd, nullptr, &s.staging)) ||
            FAILED(g.device->CreateUnorderedAccessView(s.log.Get(), &uv, &s.view)))
            return false;
    const UINT zero[4] = {};
    for (Slot& s : a.slots)
        g.context->ClearUnorderedAccessViewUint(s.view.Get(), zero);
    return true;
}

void report(const char* why, const Kind& k, const KindStat& s, uint64_t frame) {
    char b[4][64];
    for (int j = 0; j < 4; ++j)
        snprintf(b[j], sizeof(b[j]), "(%g,%g,%g,%g)", s.around[j][0], s.around[j][1], s.around[j][2], s.around[j][3]);
    simqvfr::logf("scs violet: %s at frame %llu: %s_%016llx row %u (%s), mirror %d index %u (block offset %u), draw %u "
                  "into %ux%u; vectors %s %s %s %s; seen in %llu frames (first %llu)",
                  why, static_cast<unsigned long long>(frame), k.stage ? "PS" : "VS", static_cast<unsigned long long>(k.shader),
                  k.row,
                  k.reads == 3   ? "reads both rows"
                  : k.reads == 1 ? "reads the (1,1,1,1) row ALONE"
                  : k.reads == 2 ? "reads the size row alone"
                                 : "read by no draw",
                  s.mirror, s.index, s.index & 15, s.draw, s.w, s.h, b[0], b[1], b[2], b[3],
                  static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.firstFrame));
}

// One frame's log against its draws.
void process(Slot& s, const uint8_t* data) {
    uint32_t n = 0;
    std::memcpy(&n, data, 4);
    const uint32_t kept = std::min<uint32_t>(n, kCapacity);
    a.entries += n;
    a.maxEntries = std::max<uint64_t>(a.maxEntries, n);
    a.overflowFrames += n > kCapacity ? 1 : 0;
    ++a.processedFrames;
    const bool warm = a.processedFrames > kWarmup;
    for (uint32_t e = 0; e < kept; ++e) {
        const uint8_t* p = data + kHeaderBytes + size_t{e} * kEntryBytes;
        uint32_t tag = 0, index = 0;
        std::memcpy(&tag, p, 4);
        std::memcpy(&index, p + 4, 4);
        const int mirror = static_cast<int>((tag >> 24) & 0x7f);
        const uint32_t serial = tag & 0xffffff;
        float around[4][4];
        std::memcpy(around, p + 16, sizeof(around));
        const auto note = [&](const Kind& k, const DrawRec* r) {
            KindStat& st = a.kinds[k];
            const bool fresh = st.frames == 0;
            if (st.countedFrame != s.frame) {
                st.countedFrame = s.frame;
                ++st.frames;
                st.lastFrame = s.frame;
            }
            if (!fresh)
                return;
            st.firstFrame = s.frame;
            std::memcpy(st.around, around, sizeof(around));
            st.index = index;
            st.mirror = mirror;
            if (r) {
                st.draw = r->draw;
                st.w = r->w;
                st.h = r->h;
            }
            if (warm) {
                a.recentNew.emplace_back(s.frame, k);
                if (a.reports < kMaxReports) {
                    ++a.reports;
                    report("NEW kind", k, st, s.frame);
                }
            }
        };
        bool anyRead = false;
        for (const DrawRec& r : s.draws) {
            if (r.mirror != mirror || r.serial != serial || index < r.first || index + 1 >= r.first + r.count)
                continue;
            const UINT row = index - r.first;
            const uint8_t reads = static_cast<uint8_t>((reads_row(r.shader, row) ? 1 : 0) | (reads_row(r.shader, row + 1) ? 2 : 0));
            if (!reads)
                continue;
            anyRead = true;
            note({r.stage, r.shader, row, reads}, &r);
        }
        if (!anyRead) {
            ++a.unread;
            note({0, 0, 0, 0}, nullptr);  // one kind for every pair no draw reads
        }
    }
    while (!a.recentNew.empty() && a.recentNew.front().first + kMarkWindow * 4 < s.frame)
        a.recentNew.pop_front();
}

void summary() {
    std::vector<std::pair<Kind, KindStat>> list(a.kinds.begin(), a.kinds.end());
    std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.second.frames < y.second.frames; });
    simqvfr::logf("scs violet: %llu frames read (%llu lost), %llu pairs rewritten (most in a frame %llu; frames past the "
                  "log's %u: %llu), %llu read by no draw; %zu kinds; the rarest:",
                  static_cast<unsigned long long>(a.processedFrames), static_cast<unsigned long long>(a.lost),
                  static_cast<unsigned long long>(a.entries), static_cast<unsigned long long>(a.maxEntries), kCapacity,
                  static_cast<unsigned long long>(a.overflowFrames), static_cast<unsigned long long>(a.unread), list.size());
    for (size_t i = 0; i < list.size() && i < 15; ++i)
        report("rare kind", list[i].first, list[i].second, list[i].second.lastFrame);
}

}  // namespace

void anomaly_enable(bool on) {
    a.on = on;
    if (on) {
        simqvfr::mvs::mvr_probe_cb_rows(true);  // every shader tagged with its id and the rows it reads (kCbRows4Guid)
        simqvfr::logf("scs violet: anomaly log on (scs_violet_log=1): every rewritten pair against the draws that read it; "
                      "new kinds after %llu frames logged at once, the rarest every %llu frames; Ctrl+Alt+M when you see a "
                      "flash",
                      static_cast<unsigned long long>(kWarmup), static_cast<unsigned long long>(kSummaryEvery));
    }
}

// The mirror pass's log for this frame (null: none).
ID3D11UnorderedAccessView* anomaly_uav() {
    if (!a.on || a.failed || !g.device)
        return nullptr;
    if (!a.ready) {
        a.ready = make_slots();
        if (!a.ready) {
            a.failed = true;
            simqvfr::log_line("scs violet: the log's buffers couldn't be made: off");
            return nullptr;
        }
    }
    a.slots[a.current].used = true;
    return a.slots[a.current].view.Get();
}

uint32_t anomaly_tag(const Mirror& m) {
    return 0x80000000u | (static_cast<uint32_t>(m.id & 0x7f) << 24) | (m.serial & 0xffffff);
}

// A draw while our copies are bound: its constant buffer 0 per stage, if a copy, with the range and the shader.
void anomaly_draw() {
    if (!a.on || !a.ready || !g.mirrorsBound)
        return;
    const uint32_t draw = ++a.drawNo;
    for (uint8_t stage = 0; stage < 2; ++stage) {
        ID3D11Buffer* buf = nullptr;
        UINT first = 0, num = 0;
        if (stage)
            g.context->PSGetConstantBuffers1(0, 1, &buf, &first, &num);
        else
            g.context->VSGetConstantBuffers1(0, 1, &buf, &first, &num);
        if (!buf)
            continue;
        const Mirror* m = nullptr;
        for (const Mirror& x : g.mirrors)
            if (x.copy.Get() == buf)
                m = &x;
        buf->Release();
        if (!m)
            continue;
        ComPtr<ID3D11DeviceChild> shader;
        if (stage) {
            ID3D11PixelShader* s = nullptr;
            g.context->PSGetShader(&s, nullptr, nullptr);
            shader.Attach(s);
        } else {
            ID3D11VertexShader* s = nullptr;
            g.context->VSGetShader(&s, nullptr, nullptr);
            shader.Attach(s);
        }
        DrawRec r;
        rows_of(shader.Get(), r.shader);
        r.draw = draw;
        r.mirror = m->id;
        r.serial = m->serial & 0xffffff;
        r.first = num ? first : 0;
        r.count = num ? num : m->bytes / 16;
        r.stage = stage;
        r.w = g.boundW;
        r.h = g.boundH;
        a.slots[a.current].draws.push_back(r);
    }
}

// At each frame's end (the render thread): this frame's log copied for reading, the next slot cleared, logs old enough
// read without waiting, the summary and the mark key.
void anomaly_frame_end() {
    if (!a.on || !a.ready)
        return;
    Slot& cur = a.slots[a.current];
    if (cur.used || !cur.draws.empty()) {
        cur.frame = g.stats.frames;
        g.context->CopyResource(cur.staging.Get(), cur.log.Get());
        cur.inFlight = true;
        cur.used = false;
        a.current = (a.current + 1) % kSlots;
        Slot& next = a.slots[a.current];
        if (next.inFlight) {  // still not read (the GPU that far behind): that frame is lost, counted
            next.inFlight = false;
            next.draws.clear();
            ++a.lost;
        }
        const UINT zero[4] = {};
        g.context->ClearUnorderedAccessViewUint(next.view.Get(), zero);
    }
    a.drawNo = 0;
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = a.slots[i];
        if (!s.inFlight || i == a.current || s.frame + kReadAfter > g.stats.frames)
            continue;
        D3D11_MAPPED_SUBRESOURCE map;
        if (g.context->Map(s.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map) != S_OK)
            continue;
        process(s, static_cast<const uint8_t*>(map.pData));
        g.context->Unmap(s.staging.Get(), 0);
        s.inFlight = false;
        s.draws.clear();
    }
    if (g.stats.frames >= a.lastSummary + kSummaryEvery && a.processedFrames) {
        a.lastSummary = g.stats.frames;
        summary();
    }
    if (simqvfr::kDevBuild) {
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                          (GetAsyncKeyState('M') & 0x8000);
        if (down && !a.markDown) {
            simqvfr::logf("scs violet: MARK (Ctrl+Alt+M) at frame %llu; kinds first seen in the last %llu frames:",
                          static_cast<unsigned long long>(g.stats.frames), static_cast<unsigned long long>(kMarkWindow));
            int listed = 0;
            for (const auto& [frame, k] : a.recentNew)
                if (frame + kMarkWindow >= g.stats.frames && listed < 20) {
                    ++listed;
                    report("recent", k, a.kinds[k], frame);
                }
            if (!listed)
                simqvfr::log_line("scs violet:   none new in that time; the rarest kinds follow");
            summary();
        }
        a.markDown = down;
    }
}

uint64_t anomaly_entries() { return a.entries; }
size_t anomaly_kinds() { return a.kinds.size(); }

// Session end (release_all): the device's buffers let go; the counts stay.
void anomaly_release() {
    for (Slot& s : a.slots) {
        s.log.Reset();
        s.staging.Reset();
        s.view.Reset();
        s.used = s.inFlight = false;
        s.draws.clear();
    }
    a.ready = a.failed = false;
    a.current = 0;
}

}  // namespace detail
}  // namespace scs
