// The mirror diagnostic (dev builds, [experiment] scs_mirror_diag=1): which shaders read the pairs our copies rewrite,
// and how. The copies rewrite every (1,1,1,1) followed by an eye-shaped size, by value; a white colour that looks like
// a pair's first half turned violet (2026-11-18), and the lines and signs need the real pairs. Once a second one frame
// is sampled: each refresh's original data copied to a readback buffer, and each draw's bound copies (vertex and pixel
// slots 0-3) recorded with its shader. Three frames on, the pairs are found on the CPU (the copy's own rule) and set
// against what each shader reads by a fixed index (mvr_probe's kCbRows4Guid): the (1,1,1,1) row, the size row or both.
// The totals are logged every 10 seconds, each shader row with the vectors around its first match.
#include "corner_internal.h"

#include <algorithm>
#include <tuple>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "../../engine/multi_view_shaders.h"
#include "../../log.h"

namespace scs {
namespace detail {
namespace {

constexpr uint64_t kSampleEvery = 90, kLogEvery = 900, kReadAfter = 3;

struct Copy {
    ID3D11Buffer* game = nullptr;  // the game's buffer (raw: a key only)
    uint32_t serial = 0;
    UINT bytes = 0;
    ComPtr<ID3D11Buffer> staging;
};
struct Bind {
    ID3D11Buffer* game = nullptr;
    uint32_t serial = 0;
    UINT first = 0, count = 0;
    uint8_t stage = 0, slot = 0;
    uint64_t shader = 0;
    UINT w = 0, h = 0;
};
struct Sample {
    uint64_t frame = 0;
    std::vector<Copy> copies;
    std::vector<Bind> binds;
};
struct Key {
    uint8_t stage, slot;
    uint64_t shader;
    UINT row;
    bool operator<(const Key& o) const {
        return std::tie(stage, slot, shader, row) < std::tie(o.stage, o.slot, o.shader, o.row);
    }
};
struct Stat {
    uint64_t draws = 0, readsOnes = 0, readsSize = 0, both = 0;
    UINT w = 0, h = 0;
    float around[4][4] = {};  // the vectors from the row before the (1,1,1,1) to the one after the size, first seen
};
using Rows = std::array<uint64_t, 4 * 64>;

struct Diag {
    bool on = false;
    uint64_t nextSample = 0, lastLog = 0, samples = 0, matches = 0, bindsSeen = 0;
    bool sampling = false;
    Sample current;
    std::deque<Sample> pending;
    std::vector<Copy> pool;
    std::map<uint64_t, Rows> rows;  // by shader id; all zero: unknown
    std::map<Key, Stat> stats;
} d;

bool eye_size(const float* v) {
    const float w = static_cast<float>(g.eyeW), h = static_cast<float>(g.eyeH);
    return v[2] == 1 && v[3] == 1 && v[0] >= 16 && v[0] <= w && v[1] <= h && v[0] == std::floor(v[0]) &&
           v[1] == std::floor(v[1]) && std::fabs(v[0] * h - v[1] * w) <= std::max(w, h);
}
bool ones(const float* v) { return v[0] == 1 && v[1] == 1 && v[2] == 1 && v[3] == 1; }

const Rows* rows_of(ID3D11DeviceChild* shader, uint64_t& id) {
    id = 0;
    UINT size = sizeof(id);
    if (!shader || FAILED(shader->GetPrivateData(simqvfr::mvs::kShaderIdGuid, &size, &id)))
        return nullptr;
    auto it = d.rows.find(id);
    if (it == d.rows.end()) {
        Rows r{};
        UINT rs = sizeof(r);
        if (FAILED(shader->GetPrivateData(simqvfr::mvs::kCbRows4Guid, &rs, r.data())) || rs != sizeof(r))
            r.fill(0);
        it = d.rows.emplace(id, r).first;
    }
    return &it->second;
}

bool reads(const Rows& r, unsigned slot, UINT row) { return row < 4096 && ((r[slot * 64 + row / 64] >> (row % 64)) & 1) != 0; }

void process(Sample& s) {
    // Each copy's pairs: the (1,1,1,1) rows, as the copies' own rule finds them (the size in the same 16-vector block).
    std::map<std::pair<ID3D11Buffer*, uint32_t>, std::vector<UINT>> found;
    std::map<std::pair<ID3D11Buffer*, uint32_t>, std::vector<float>> data;
    for (Copy& c : s.copies) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g.context->Map(c.staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
            continue;
        const auto* v = static_cast<const float*>(m.pData);
        const UINT n = c.bytes / 16;
        std::vector<UINT>& f = found[{c.game, c.serial}];
        for (UINT i = 0; i + 1 < n; ++i)
            if (((i + 1) & 15) != 0 && ones(v + i * 4) && eye_size(v + (i + 1) * 4))
                f.push_back(i);
        d.matches += f.size();
        data[{c.game, c.serial}].assign(v, v + n * 4);
        g.context->Unmap(c.staging.Get(), 0);
        d.pool.push_back(std::move(c));
    }
    for (const Bind& b : s.binds) {
        auto it = found.find({b.game, b.serial});
        if (it == found.end())
            continue;
        const auto r = d.rows.find(b.shader);
        if (r == d.rows.end())
            continue;
        const std::vector<float>& v = data[{b.game, b.serial}];
        for (UINT o : it->second) {
            if (o < b.first || o + 1 >= b.first + b.count)
                continue;
            const UINT k = o - b.first;
            const bool a = reads(r->second, b.slot, k), z = reads(r->second, b.slot, k + 1);
            if (!a && !z)
                continue;
            Stat& st = d.stats[{b.stage, b.slot, b.shader, k}];
            if (st.draws == 0)
                for (int j = 0; j < 4; ++j) {
                    const int at = static_cast<int>(o) - 1 + j;
                    if (at >= 0 && static_cast<size_t>(at) * 4 + 3 < v.size())
                        std::memcpy(st.around[j], &v[static_cast<size_t>(at) * 4], 16);
                }
            ++st.draws;
            st.readsOnes += a ? 1 : 0;
            st.readsSize += z ? 1 : 0;
            st.both += a && z ? 1 : 0;
            st.w = b.w;
            st.h = b.h;
        }
    }
}

void log_stats() {
    std::vector<std::pair<Key, Stat>> list(d.stats.begin(), d.stats.end());
    std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.second.draws > y.second.draws; });
    simqvfr::logf("scs mirror diag: %llu frames sampled, %llu pairs found, %llu draws read a mirror; %zu shader rows read a "
                  "rewritten pair (draws; reading the (1,1,1,1) row / the size row / both; the target; the vectors from "
                  "the one before the (1,1,1,1) to the one after the size, first seen):",
                  static_cast<unsigned long long>(d.samples), static_cast<unsigned long long>(d.matches),
                  static_cast<unsigned long long>(d.bindsSeen), list.size());
    for (size_t i = 0; i < list.size() && i < 40; ++i) {
        const Key& k = list[i].first;
        const Stat& s = list[i].second;
        const auto vec = [&](int j) {
            static thread_local char b[4][64];
            snprintf(b[j], sizeof(b[j]), "(%g,%g,%g,%g)", s.around[j][0], s.around[j][1], s.around[j][2], s.around[j][3]);
            return b[j];
        };
        simqvfr::logf("scs mirror diag:   %s_%016llx slot %u row %u: %llu draws; %llu / %llu / %llu; %ux%u; %s %s %s %s",
                      k.stage ? "PS" : "VS", static_cast<unsigned long long>(k.shader), k.slot, k.row,
                      static_cast<unsigned long long>(s.draws), static_cast<unsigned long long>(s.readsOnes),
                      static_cast<unsigned long long>(s.readsSize), static_cast<unsigned long long>(s.both), s.w, s.h,
                      vec(0), vec(1), vec(2), vec(3));
    }
}

}  // namespace

void mirror_diag_enable(bool on) {
    d.on = on;
    simqvfr::mvs::mvr_probe_cb_rows(on);
    if (on)
        simqvfr::logf("scs mirror diag: on (scs_mirror_diag=1): one frame in %llu sampled, the totals every %llu frames",
                      static_cast<unsigned long long>(kSampleEvery), static_cast<unsigned long long>(kLogEvery));
}

void mirror_diag_refresh(const Mirror& m) {
    if (!d.on || !d.sampling)
        return;
    Copy c;
    for (auto it = d.pool.begin(); it != d.pool.end(); ++it)
        if (it->bytes == m.bytes) {
            c = std::move(*it);
            d.pool.erase(it);
            break;
        }
    if (!c.staging) {
        const D3D11_BUFFER_DESC sd = {m.bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        if (FAILED(g.device->CreateBuffer(&sd, nullptr, &c.staging)))
            return;
        c.bytes = m.bytes;
    }
    c.game = m.game.Get();
    c.serial = m.serial;
    const D3D11_BOX box = {0, 0, 0, m.bytes, 1, 1};
    g.context->CopySubresourceRegion(c.staging.Get(), 0, 0, 0, 0, m.game.Get(), 0, &box);
    d.current.copies.push_back(std::move(c));
}

void mirror_diag_draw() {
    if (!d.on || !d.sampling || !g.mirrorsBound)
        return;
    for (uint8_t stage = 0; stage < 2; ++stage) {
        ID3D11Buffer* bufs[4] = {};
        UINT first[4] = {}, num[4] = {};
        ComPtr<ID3D11DeviceChild> shader;
        if (stage) {
            g.context->PSGetConstantBuffers1(0, 4, bufs, first, num);
            ID3D11PixelShader* s = nullptr;
            g.context->PSGetShader(&s, nullptr, nullptr);
            shader.Attach(s);
        } else {
            g.context->VSGetConstantBuffers1(0, 4, bufs, first, num);
            ID3D11VertexShader* s = nullptr;
            g.context->VSGetShader(&s, nullptr, nullptr);
            shader.Attach(s);
        }
        uint64_t id = 0;
        const Rows* r = rows_of(shader.Get(), id);
        for (uint8_t slot = 0; slot < 4; ++slot) {
            if (!bufs[slot])
                continue;
            if (r)
                for (const Mirror& m : g.mirrors)
                    if (m.copy.Get() == bufs[slot]) {
                        Bind b;
                        b.game = m.game.Get();
                        b.serial = m.serial;
                        b.first = num[slot] ? first[slot] : 0;
                        b.count = num[slot] ? num[slot] : m.bytes / 16;
                        b.stage = stage;
                        b.slot = slot;
                        b.shader = id;
                        b.w = g.boundW;
                        b.h = g.boundH;
                        d.current.binds.push_back(b);
                        ++d.bindsSeen;
                        break;
                    }
            bufs[slot]->Release();
        }
    }
}

void mirror_diag_frame_end() {
    if (!d.on)
        return;
    const uint64_t now = g.stats.frames;
    if (d.sampling && now > d.current.frame) {
        d.sampling = false;
        d.pending.push_back(std::move(d.current));
        d.current = Sample{};
        ++d.samples;
    }
    while (!d.pending.empty() && d.pending.front().frame + kReadAfter <= now) {
        process(d.pending.front());
        d.pending.pop_front();
    }
    if (!d.sampling && now >= d.nextSample) {
        d.sampling = true;
        d.current.frame = now;
        d.nextSample = now + kSampleEvery;
    }
    if (now >= d.lastLog + kLogEvery && !d.stats.empty()) {
        d.lastLog = now;
        log_stats();
    }
}

}  // namespace detail
}  // namespace scs
