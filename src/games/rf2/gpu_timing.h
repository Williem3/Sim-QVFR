#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "../../log.h"

namespace simqvfr::games::rf2 {

// GPU time of RF2's eye work, from D3D11 timestamp queries on the tracked immediate context (render thread only: every
// call comes from a hook that already runs there). Log-only: it never changes what is drawn.
//
// A frame is the stretch from its first scene-target bind to the next frame's. In it, a stamp at:
//   - each scene pass's start and end (the bind of the full scene target with depth, and the bind that leaves it),
//   - each scene resolve's start and end, and the end of the composition made after it (compositor.h).
// Frames are kept in a ring of query sets and read back a few frames later without waiting (DONOTFLUSH), so the
// measurement costs the game no stall. The timestamps are GPU wall-clock: a segment includes any time the GPU waited
// (starved by the CPU), which is why the frame period and the segments are logged side by side.
//
// [experiment] gpu_timing=2 also counts, per scene pass, what the pipeline did (D3D11 pipeline-statistics queries around the
// pass): pixel-shader invocations, vertex-shader invocations and primitives that got past clipping. Next to the pass's
// milliseconds that says whether a pass is pixel-bound (its time follows its pixel count) or vertex-bound (it follows the
// vertices), without a graphics debugger: the log gives each pass's typical numbers, its pixel throughput, and the
// correlation of its time with each count over the window's frames. It costs a little GPU time of its own, so it is for the
// measuring runs.
//
// Every kWindow frames one summary (a few lines) goes to the log: the frame period (open to open), the scene span (first pass
// start to last stamp), each pass, the resolve, the composition, the time between the stamped parts, and for the slow
// frames (period over 1.5 x the median) their averages next to the median's, so a missed vsync reads as "more GPU work" or
// "the same work, started late".
class GpuTiming {
public:
    static constexpr int kPasses = 10;  // scene passes of a frame, numbered from 1 (PassState::fullScenePassInFrame)
    static constexpr int kResolves = 4;  // scene resolves of a frame
    static constexpr int kSlots = 8;     // frames in flight in the ring
    static constexpr unsigned kWindow = 300;

    // 0 off, 1 timing, 2 timing and the passes' pipeline statistics ([experiment] gpu_timing, default 0: off). The layer's
    // thread sets it; the render thread takes it up at the next frame (sync), so no query is made or freed on another thread.
    void set_mode(int mode) { modeWanted_ = std::clamp(mode, 0, 2); }
    // The GPU regulator wants the timing (every frame's span goes to the sink) whether or not the log is on.
    void set_need(bool need) { needWanted_ = need; }
    // Called with each timed frame's GPU time in ms (the scene span), on the render thread.
    void set_sink(std::function<void(double)> sink) { sink_ = std::move(sink); }
    bool enabled() const { return enabled_; }

    // The first scene pass of a frame is about to begin: the last frame's stamps are closed and read, this frame's begin.
    void frame_open(ID3D11DeviceContext* c) {
        sync(c);
        if (!enabled_ || !c) return;
        if (c != ctx_) rebuild(c);
        if (!ok_) return;
        collect(c);
        close_current(c);
        for (int i = 0; i < kSlots; ++i) {
            const int index = (next_ + i) % kSlots;
            Slot& s = slots_[index];
            if (s.open || s.pending) continue;
            next_ = (index + 1) % kSlots;
            std::memset(s.used, 0, sizeof(s.used));
            std::memset(s.statOpen, 0, sizeof(s.statOpen));
            std::memset(s.statEnded, 0, sizeof(s.statEnded));
            s.tries = 0;
            s.seq = ++seq_;
            c->Begin(s.disjoint.Get());
            s.open = true;
            current_ = index;
            resolves_ = 0;
            currentResolve_ = -1;
            stamp(c, kOpen);
            return;
        }
        ++dropped_;  // every slot still waits for its read-back: this frame is not timed (its neighbours have no period)
    }

    void pass_open(ID3D11DeviceContext* c, unsigned pass) {
        if (pass < 1 || pass > static_cast<unsigned>(kPasses)) return;
        stamp(c, pass_open_id(pass));
        if (statOk_ && current_ >= 0) {
            end_stat(c);  // one pipeline-statistics query is active at a time: a pass left open ends here
            Slot& s = slots_[current_];
            if (s.open && !s.statEnded[pass] && !s.statOpen[pass]) {
                c->Begin(s.stat[pass].Get());
                s.statOpen[pass] = true;
                activeStat_ = static_cast<int>(pass);
            }
        }
    }
    void pass_close(ID3D11DeviceContext* c, unsigned pass) {
        if (pass < 1 || pass > static_cast<unsigned>(kPasses)) return;
        stamp(c, pass_open_id(pass) + 1);
        if (activeStat_ == static_cast<int>(pass)) end_stat(c);
    }
    // A scene resolve (ResolveSubresource of the scene target), before and after the real call.
    void resolve_begin(ID3D11DeviceContext* c) {
        if (current_ < 0) return;
        currentResolve_ = resolves_ < kResolves ? resolves_ : -1;
        ++resolves_;
        if (currentResolve_ >= 0) stamp(c, resolve_id(currentResolve_));
    }
    void resolve_done(ID3D11DeviceContext* c) {
        if (currentResolve_ >= 0) stamp(c, resolve_id(currentResolve_) + 1);
    }
    // The composition after that resolve is made.
    void compose_done(ID3D11DeviceContext* c) {
        if (currentResolve_ >= 0) stamp(c, resolve_id(currentResolve_) + 2);
    }

private:
    static constexpr int kOpen = 0;
    static constexpr int kResolveBase = 1 + 2 * kPasses;
    static constexpr int kStamps = kResolveBase + 3 * kResolves;
    static int pass_open_id(unsigned pass) { return 1 + 2 * static_cast<int>(pass - 1); }
    static int resolve_id(int k) { return kResolveBase + 3 * k; }

    // The wanted mode and need, taken up on the render thread: the queries are made again when what runs changes.
    void sync(ID3D11DeviceContext* c) {
        const int mode = modeWanted_.load();
        const bool need = needWanted_.load();
        if (configured_ && mode == mode_ && need == need_) return;
        const bool modeChanged = !configured_ || mode != mode_;
        configured_ = true;
        mode_ = mode;
        need_ = need;
        if (modeChanged)
            simqvfr::logf("rf2 gpu timing: %s ([experiment] gpu_timing=%d; a summary every %u frames)",
                          mode == 0 ? "log off" : mode == 1 ? "log on" : "log on, with the passes' pipeline statistics", mode,
                          kWindow);
        if (need && !need_logged_) {
            need_logged_ = true;
            simqvfr::log_line("rf2 gpu timing: running for the GPU regulator");
        }
        enabled_ = mode > 0 || need;
        if (c && c == ctx_) close_current(c);  // a frame's open query is ended before its queries are let go
        reset();  // the queries are made again (with or without the statistics) at the next frame
    }

    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> ts[kStamps];
        bool used[kStamps] = {};
        Microsoft::WRL::ComPtr<ID3D11Query> stat[kPasses + 1];  // pipeline statistics of a pass (gpu_timing=2), by pass number
        bool statOpen[kPasses + 1] = {}, statEnded[kPasses + 1] = {};
        bool open = false, pending = false;
        unsigned tries = 0;  // read-back attempts that found a stamp not ready
        unsigned long long seq = 0;
    };

    struct Sample {
        unsigned long long seq = 0;
        double period = -1;  // this frame's open to the next frame's open: how long the frame took (-1: not known)
        double span = 0, passTotal = 0, resolve = 0, compose = 0, between = 0;
        double pass[kPasses + 1] = {};  // ms, index = pass number; -1 when the pass was not stamped
        // The pass's pipeline statistics (gpu_timing=2), -1 when not counted: pixel-shader and vertex-shader invocations,
        // primitives past clipping; the frame's sums in psTotal, vsTotal, primTotal (-1: none counted).
        double ps[kPasses + 1], vs[kPasses + 1], prim[kPasses + 1];
        double psTotal = -1, vsTotal = -1, primTotal = -1;
        Sample() {
            for (int i = 0; i <= kPasses; ++i) ps[i] = vs[i] = prim[i] = -1;
        }
    };

    void stamp(ID3D11DeviceContext* c, int id) {
        if (current_ < 0 || id < 0 || id >= kStamps) return;
        Slot& s = slots_[current_];
        if (!s.open || s.used[id]) return;
        c->End(s.ts[id].Get());
        s.used[id] = true;
    }

    // Ends the active pass's statistics query (the slot's, current_).
    void end_stat(ID3D11DeviceContext* c) {
        if (activeStat_ < 0 || current_ < 0) {
            activeStat_ = -1;
            return;
        }
        Slot& s = slots_[current_];
        c->End(s.stat[activeStat_].Get());
        s.statOpen[activeStat_] = false;
        s.statEnded[activeStat_] = true;
        activeStat_ = -1;
    }

    void close_current(ID3D11DeviceContext* c) {
        end_stat(c);  // a pass that never closed
        if (current_ < 0) return;
        Slot& s = slots_[current_];
        if (s.open) {
            c->End(s.disjoint.Get());
            s.open = false;
            s.pending = true;
        }
        current_ = -1;
    }

    void reset() {
        for (Slot& s : slots_) {
            s.disjoint.Reset();
            for (auto& q : s.ts) q.Reset();
            for (auto& q : s.stat) q.Reset();
            s.open = s.pending = false;
        }
        ctx_ = nullptr;
        ok_ = false;
        statOk_ = false;
        activeStat_ = -1;
        current_ = -1;
        prevValid_ = false;
        haveHeld_ = false;
        window_.clear();
    }

    // A new tracked context (the game made a new device at xrCreateSession): the queries are made on its device.
    void rebuild(ID3D11DeviceContext* c) {
        reset();
        ctx_ = c;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        c->GetDevice(&device);
        ok_ = device != nullptr;
        const D3D11_QUERY_DESC dq = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, tq = {D3D11_QUERY_TIMESTAMP, 0};
        for (Slot& s : slots_) {
            if (!ok_) break;
            ok_ = SUCCEEDED(device->CreateQuery(&dq, &s.disjoint));
            for (auto& q : s.ts) ok_ = ok_ && SUCCEEDED(device->CreateQuery(&tq, &q));
        }
        simqvfr::logf("rf2 gpu timing: timestamp queries %s on context %p", ok_ ? "ready" : "could not be made", static_cast<void*>(c));
        if (!ok_) enabled_ = false;
        if (ok_ && mode_ >= 2) {
            const D3D11_QUERY_DESC sq = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
            statOk_ = true;
            for (Slot& s : slots_)
                for (int p = 1; p <= kPasses && statOk_; ++p) statOk_ = SUCCEEDED(device->CreateQuery(&sq, &s.stat[p]));
            if (!statOk_)
                for (Slot& s : slots_)
                    for (auto& q : s.stat) q.Reset();
            simqvfr::logf("rf2 gpu timing: pipeline-statistics queries %s", statOk_ ? "ready" : "could not be made (timing only)");
        }
    }

    // Frames whose queries have finished, oldest first.
    void collect(ID3D11DeviceContext* c) {
        for (;;) {
            int oldest = -1;
            for (int i = 0; i < kSlots; ++i)
                if (slots_[i].pending && (oldest < 0 || slots_[i].seq < slots_[oldest].seq)) oldest = i;
            if (oldest < 0) return;
            Slot& s = slots_[oldest];
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (c->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;  // not yet: the later ones are not either
            UINT64 t[kStamps] = {};
            D3D11_QUERY_DATA_PIPELINE_STATISTICS st[kPasses + 1] = {};
            bool ok = !dj.Disjoint && dj.Frequency != 0;
            bool waiting = false;  // a result not ready although the frame's query is: asked again later, up to a point
            const auto read = [&](ID3D11Query* q, void* data, UINT size) {
                const HRESULT r = c->GetData(q, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (r == S_FALSE) waiting = true;
                else if (r != S_OK) ok = false;
            };
            for (int k = 0; k < kStamps && ok && !waiting; ++k)
                if (s.used[k]) read(s.ts[k].Get(), &t[k], sizeof(UINT64));
            for (int p = 1; p <= kPasses && ok && !waiting; ++p)
                if (s.statEnded[p]) read(s.stat[p].Get(), &st[p], sizeof(st[p]));
            if (waiting && ok && ++s.tries < 30) return;
            if (waiting) ok = false;  // given up on
            s.pending = false;
            if (!ok) {
                if (!dj.Disjoint) ++dropped_;  // a disjoint frame (the GPU's clock changed) is not a loss of ours
                prevValid_ = false;
                flush_held();
                continue;
            }
            add_sample(s, t, dj.Frequency, st);
        }
    }

    void add_sample(const Slot& s, const UINT64* t, UINT64 frequency, const D3D11_QUERY_DATA_PIPELINE_STATISTICS* st) {
        const double toMs = 1000.0 / static_cast<double>(frequency);
        const auto ms = [&](int from, int to) { return t[to] >= t[from] ? static_cast<double>(t[to] - t[from]) * toMs : 0.0; };
        Sample m;
        m.seq = s.seq;
        // A frame's period is known when the next one has opened: the frame waits (held_) for it.
        const bool adjacent = prevValid_ && prevSeq_ + 1 == s.seq && t[kOpen] >= prevOpen_ && prevFrequency_ == frequency;
        if (haveHeld_) {
            if (adjacent) held_.period = static_cast<double>(t[kOpen] - prevOpen_) * toMs;
            push(held_);
            haveHeld_ = false;
        }
        prevValid_ = true;
        prevSeq_ = s.seq;
        prevOpen_ = t[kOpen];
        prevFrequency_ = frequency;

        UINT64 first = 0, last = 0;
        bool any = false;
        for (unsigned p = 1; p <= static_cast<unsigned>(kPasses); ++p) {
            m.pass[p] = -1;
            const int open = pass_open_id(p), close = open + 1;
            if (!s.used[open]) continue;
            if (!any || t[open] < first) first = t[open];
            any = true;
            if (s.used[close]) m.pass[p] = ms(open, close);
        }
        for (int k = 0; k < kStamps; ++k)
            if (k != kOpen && s.used[k] && (t[k] > last)) last = t[k];
        if (!any) return;  // no pass stamped: nothing to say about the scene (the next frame has no period either way)
        m.span = last >= first ? static_cast<double>(last - first) * toMs : 0.0;
        if (sink_) sink_(m.span);
        if (mode_ == 0) return;  // timing only for the regulator: nothing is logged
        for (unsigned p = 1; p <= static_cast<unsigned>(kPasses); ++p)
            if (m.pass[p] > 0) m.passTotal += m.pass[p];
        for (int k = 0; k < kResolves; ++k) {
            const int base = resolve_id(k);
            if (s.used[base] && s.used[base + 1]) m.resolve += ms(base, base + 1);
            if (s.used[base + 1] && s.used[base + 2]) m.compose += ms(base + 1, base + 2);
        }
        m.between = std::max(0.0, m.span - m.passTotal - m.resolve - m.compose);
        for (unsigned p = 1; p <= static_cast<unsigned>(kPasses); ++p) {
            if (!s.statEnded[p]) continue;
            m.ps[p] = static_cast<double>(st[p].PSInvocations);
            m.vs[p] = static_cast<double>(st[p].VSInvocations);
            m.prim[p] = static_cast<double>(st[p].CPrimitives);
            m.psTotal = (m.psTotal < 0 ? 0 : m.psTotal) + m.ps[p];
            m.vsTotal = (m.vsTotal < 0 ? 0 : m.vsTotal) + m.vs[p];
            m.primTotal = (m.primTotal < 0 ? 0 : m.primTotal) + m.prim[p];
        }
        held_ = m;
        haveHeld_ = true;
    }

    void push(const Sample& m) {
        window_.push_back(m);
        if (window_.size() >= kWindow) log_window();
    }

    // A frame whose successor is lost (not timed, or a different clock): it goes in without a period.
    void flush_held() {
        if (!haveHeld_) return;
        haveHeld_ = false;
        push(held_);
    }

    static double percentile(std::vector<double> v, double q) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5)];
    }

    // Pearson correlation of two columns of equal length; NaN when there are too few points or one does not vary.
    static double correlation(const std::vector<double>& x, const std::vector<double>& y) {
        const size_t n = std::min(x.size(), y.size());
        if (n < 20) return std::nan("");
        double mx = 0, my = 0;
        for (size_t i = 0; i < n; ++i) {
            mx += x[i];
            my += y[i];
        }
        mx /= static_cast<double>(n);
        my /= static_cast<double>(n);
        double sxx = 0, syy = 0, sxy = 0;
        for (size_t i = 0; i < n; ++i) {
            sxx += (x[i] - mx) * (x[i] - mx);
            syy += (y[i] - my) * (y[i] - my);
            sxy += (x[i] - mx) * (y[i] - my);
        }
        if (sxx < 1e-12 || syy < 1e-12) return std::nan("");
        return sxy / std::sqrt(sxx * syy);
    }

    template <class F>
    static std::vector<double> column(const std::vector<Sample>& w, F&& f) {
        std::vector<double> v;
        v.reserve(w.size());
        for (const Sample& m : w) {
            const double x = f(m);
            if (x >= 0) v.push_back(x);
        }
        return v;
    }

    // One line per pass with statistics: what it did in a typical frame (p50), how fast it did it, and whether its time follows
    // its pixels or its vertices across the window's frames (r near +1: it does; near 0: it does not; n/a: too few frames or
    // a count that does not vary).
    void log_pass_statistics(const std::vector<Sample>& w) {
        for (unsigned p = 1; p <= static_cast<unsigned>(kPasses); ++p) {
            std::vector<double> ms, ps, vs, prim, rate;
            for (const Sample& m : w) {
                if (m.pass[p] <= 0 || m.ps[p] < 0) continue;
                ms.push_back(m.pass[p]);
                ps.push_back(m.ps[p] / 1e6);
                vs.push_back(m.vs[p] / 1e3);
                prim.push_back(m.prim[p] / 1e3);
                rate.push_back(m.ps[p] / 1e6 / m.pass[p]);  // million pixel-shader invocations per ms of the pass
            }
            if (ms.size() < w.size() / 4) continue;
            const double rPs = correlation(ms, ps), rVs = correlation(ms, vs);
            char rp[16], rv[16];
            if (std::isnan(rPs)) snprintf(rp, sizeof(rp), "n/a"); else snprintf(rp, sizeof(rp), "%+.2f", rPs);
            if (std::isnan(rVs)) snprintf(rv, sizeof(rv), "n/a"); else snprintf(rv, sizeof(rv), "%+.2f", rVs);
            simqvfr::logf("rf2 gpu pass %u work (p50, %zu frames): %.2f ms, PS %.2fM (%.2f M/ms), VS %.0fk, prims %.0fk; PS p95 %.2fM max %.2fM; "
                          "r(ms,PS) %s r(ms,VS) %s",
                          p, ms.size(), percentile(ms, 0.5), percentile(ps, 0.5), percentile(rate, 0.5), percentile(vs, 0.5),
                          percentile(prim, 0.5), percentile(ps, 0.95), percentile(ps, 1.0), rp, rv);
        }
    }

    void log_window() {
        const std::vector<Sample>& w = window_;
        const auto period = column(w, [](const Sample& m) { return m.period; });
        const auto span = column(w, [](const Sample& m) { return m.span; });
        const auto resolve = column(w, [](const Sample& m) { return m.resolve; });
        const auto compose = column(w, [](const Sample& m) { return m.compose; });
        const auto between = column(w, [](const Sample& m) { return m.between; });
        const double periodP50 = percentile(period, 0.5);
        unsigned slow = 0;
        for (double p : period)
            if (p > 1.5 * periodP50) ++slow;
        simqvfr::logf("rf2 gpu: %zu frames (%u not timed so far): frame period p50 %.2f p95 %.2f max %.2f ms, %u slow (> 1.5 x p50); "
                      "scene span p50 %.2f p95 %.2f max %.2f; resolve p50 %.2f max %.2f; compose p50 %.2f max %.2f; between the "
                      "stamped parts p50 %.2f max %.2f",
                      w.size(), dropped_, periodP50, percentile(period, 0.95), percentile(period, 1.0), slow,
                      percentile(span, 0.5), percentile(span, 0.95), percentile(span, 1.0), percentile(resolve, 0.5),
                      percentile(resolve, 1.0), percentile(compose, 0.5), percentile(compose, 1.0), percentile(between, 0.5),
                      percentile(between, 1.0));

        std::string passes;
        for (unsigned p = 1; p <= static_cast<unsigned>(kPasses); ++p) {
            const auto v = column(w, [p](const Sample& m) { return m.pass[p]; });
            if (v.size() < w.size() / 4) continue;  // a pass most frames do not have
            char text[96];
            snprintf(text, sizeof(text), " p%u %.2f/%.2f/%.2f (%zu)", p, percentile(v, 0.5), percentile(v, 0.95), percentile(v, 1.0), v.size());
            passes += text;
        }
        simqvfr::logf("rf2 gpu passes (ms p50/p95/max (frames having it)):%s", passes.c_str());
        if (statOk_) log_pass_statistics(w);

        if (slow) {
            double sp = 0, ss = 0, sPass = 0, sRes = 0, sCmp = 0, sBet = 0;
            unsigned n = 0;
            for (const Sample& m : w) {
                if (m.period <= 1.5 * periodP50) continue;
                sp += m.period;
                ss += m.span;
                sPass += m.passTotal;
                sRes += m.resolve;
                sCmp += m.compose;
                sBet += m.between;
                ++n;
            }
            const auto passTotal = column(w, [](const Sample& m) { return m.passTotal; });
            simqvfr::logf("rf2 gpu slow frames (%u): averages period %.2f span %.2f passes %.2f resolve %.2f compose %.2f between %.2f; "
                          "all frames' medians: span %.2f passes %.2f resolve %.2f compose %.2f between %.2f",
                          n, sp / n, ss / n, sPass / n, sRes / n, sCmp / n, sBet / n, percentile(span, 0.5),
                          percentile(passTotal, 0.5), percentile(resolve, 0.5), percentile(compose, 0.5), percentile(between, 0.5));
            if (statOk_) {
                double sPs = 0, sVs = 0, sPr = 0;
                unsigned ns = 0;
                for (const Sample& m : w) {
                    if (m.period <= 1.5 * periodP50 || m.psTotal < 0) continue;
                    sPs += m.psTotal;
                    sVs += m.vsTotal;
                    sPr += m.primTotal;
                    ++ns;
                }
                const auto psAll = column(w, [](const Sample& m) { return m.psTotal / 1e6; });
                const auto vsAll = column(w, [](const Sample& m) { return m.vsTotal / 1e3; });
                const auto prAll = column(w, [](const Sample& m) { return m.primTotal / 1e3; });
                if (ns)
                    simqvfr::logf("rf2 gpu slow frames' work (%u counted): PS %.2fM VS %.0fk prims %.0fk; all frames' medians: PS %.2fM VS %.0fk prims %.0fk",
                                  ns, sPs / ns / 1e6, sVs / ns / 1e3, sPr / ns / 1e3, percentile(psAll, 0.5), percentile(vsAll, 0.5),
                                  percentile(prAll, 0.5));
            }
        }

        const Sample* worst = nullptr;
        for (const Sample& m : w)
            if (!worst || (m.period > worst->period) || (m.period < 0 && worst->period < 0 && m.span > worst->span)) worst = &m;
        if (worst) {
            std::string parts;
            for (unsigned p = 1; p <= static_cast<unsigned>(kPasses); ++p) {
                if (worst->pass[p] < 0) continue;
                char text[32];
                snprintf(text, sizeof(text), " p%u %.2f", p, worst->pass[p]);
                parts += text;
            }
            simqvfr::logf("rf2 gpu worst frame (#%llu): period %.2f span %.2f; passes%s; resolve %.2f compose %.2f between %.2f",
                          worst->seq, worst->period, worst->span, parts.c_str(), worst->resolve, worst->compose, worst->between);
            if (statOk_ && worst->psTotal >= 0) {
                std::string work;
                for (unsigned p = 1; p <= static_cast<unsigned>(kPasses); ++p) {
                    if (worst->ps[p] < 0) continue;
                    char text[64];
                    snprintf(text, sizeof(text), " p%u PS %.2fM VS %.0fk", p, worst->ps[p] / 1e6, worst->vs[p] / 1e3);
                    work += text;
                }
                simqvfr::logf("rf2 gpu worst frame's work:%s", work.c_str());
            }
        }
        window_.clear();
    }

    bool enabled_ = false, configured_ = false, ok_ = false, statOk_ = false, need_ = false, need_logged_ = false;
    int mode_ = 0;
    std::atomic<int> modeWanted_{0};
    std::atomic<bool> needWanted_{false};
    std::function<void(double)> sink_;
    int activeStat_ = -1;  // the pass whose statistics query is active (current_'s slot), -1 none
    ID3D11DeviceContext* ctx_ = nullptr;  // compared only
    Slot slots_[kSlots];
    int next_ = 0, current_ = -1;
    unsigned long long seq_ = 0;
    int resolves_ = 0, currentResolve_ = -1;
    unsigned dropped_ = 0;
    bool prevValid_ = false;
    unsigned long long prevSeq_ = 0;
    UINT64 prevOpen_ = 0, prevFrequency_ = 0;
    std::vector<Sample> window_;
    Sample held_;
    bool haveHeld_ = false;
};

}  // namespace simqvfr::games::rf2
