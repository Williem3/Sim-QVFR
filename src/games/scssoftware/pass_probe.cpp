#include "pass_probe.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

#include "../../engine/context_hooks.h"
#include "../../log.h"

namespace scs {

// ---- PassTracker ----

const char* PassTracker::end_name(End end) {
    switch (end) {
        case End::nextGbuffer: return "the next G-buffer";
        case End::post: return "a quarter-size target (post)";
        case End::frameEnd: return "the frame end";
        default: return "open";
    }
}

PassTracker::Segment& PassTracker::current() {
    if (phase_ == Phase::pass && frame_.passes > 0)
        return frame_.pass[frame_.passes - 1];
    return phase_ == Phase::post ? frame_.post : frame_.shared;
}

void PassTracker::close_pass(End why, double seconds) {
    Segment& s = current();
    if (segmentStart_ >= 0)
        s.ms += (seconds - segmentStart_) * 1000.0;
    segmentStart_ = seconds;
    if (phase_ == Phase::pass && frame_.passes > 0)
        frame_.end[frame_.passes - 1] = why;
}

void PassTracker::bind(const Bind& b, double seconds) {
    if (segmentStart_ < 0)
        segmentStart_ = seconds;
    const bool gbufferShape = gbuffer_shape(b);
    if (gbufferShape && b.width * b.height > eyeWidth_ * eyeHeight_) {  // the eye size, learned
        eyeWidth_ = b.width;
        eyeHeight_ = b.height;
    }
    const bool gbuffer = gbufferShape && b.width == eyeWidth_ && b.height == eyeHeight_;
    if (gbuffer) {
        if (phase_ == Phase::pass && b.first == passTarget_)
            return;  // the same pass binding its G-buffer again
        close_pass(phase_ == Phase::pass ? End::nextGbuffer : End::none, seconds);
        if (frame_.passes < kMaxPasses) {
            ++frame_.passes;
            phase_ = Phase::pass;
        } else {
            phase_ = Phase::post;  // more passes than kept: counted as post
        }
        passTarget_ = b.first;
        return;
    }
    if (phase_ == Phase::pass && b.width > 0 && b.width * 3 < eyeWidth_) {
        close_pass(End::post, seconds);
        phase_ = Phase::post;
    }
}

void PassTracker::draw(bool instanced) {
    Segment& s = current();
    ++s.draws;
    s.instanced += instanced ? 1 : 0;
}

PassTracker::Frame PassTracker::end_frame(double seconds) {
    if (segmentStart_ >= 0)
        close_pass(End::frameEnd, seconds);
    Frame done = frame_;
    done.eyeWidth = eyeWidth_;
    done.eyeHeight = eyeHeight_;
    frame_ = Frame{};
    phase_ = Phase::shared;
    passTarget_ = nullptr;
    segmentStart_ = segmentStart_ >= 0 ? seconds : -1;
    return done;
}

void PassSummary::add(const PassTracker::Frame& f) {
    ++frames;
    ++byPasses[std::min(f.passes, PassTracker::kMaxPasses)];
    if (f.passes != 2)
        return;
    ++stereo;
    sharedMs += f.shared.ms;
    sharedDraws += static_cast<double>(f.shared.draws);
    postMs += f.post.ms;
    postDraws += static_cast<double>(f.post.draws);
    for (int p = 0; p < 2; ++p) {
        passMs[p] += f.pass[p].ms;
        maxPassMs[p] = std::max(maxPassMs[p], f.pass[p].ms);
        passDraws[p] += static_cast<double>(f.pass[p].draws);
        passInstanced[p] += static_cast<double>(f.pass[p].instanced);
        ++ends[p][static_cast<int>(f.end[p])];
    }
}

std::string PassSummary::line() const {
    if (!frames)
        return "";
    char buf[768];
    if (!stereo) {
        std::snprintf(buf, sizeof(buf), "%llu frames, none with two eye passes (by passes 0/1/2/3/4: %llu/%llu/%llu/%llu/%llu)",
                      static_cast<unsigned long long>(frames), static_cast<unsigned long long>(byPasses[0]),
                      static_cast<unsigned long long>(byPasses[1]), static_cast<unsigned long long>(byPasses[2]),
                      static_cast<unsigned long long>(byPasses[3]), static_cast<unsigned long long>(byPasses[4]));
        return buf;
    }
    const double n = static_cast<double>(stereo);
    const auto ends_of = [&](int p) {
        char e[160];
        std::snprintf(e, sizeof(e), "next G-buffer %llu, post %llu, frame end %llu",
                      static_cast<unsigned long long>(ends[p][1]), static_cast<unsigned long long>(ends[p][2]),
                      static_cast<unsigned long long>(ends[p][3]));
        return std::string(e);
    };
    std::snprintf(buf, sizeof(buf),
                  "%llu frames, %llu with two eye passes (others by passes 0/1/3/4: %llu/%llu/%llu/%llu); per stereo frame "
                  "on the render thread: shared %.2f ms %.0f draws | eye A %.2f ms (max %.2f) %.0f draws (%.0f instanced) | "
                  "eye B %.2f ms (max %.2f) %.0f draws (%.0f instanced) | post+rest %.2f ms %.0f draws; ends A: %s; B: %s",
                  static_cast<unsigned long long>(frames), static_cast<unsigned long long>(stereo),
                  static_cast<unsigned long long>(byPasses[0]), static_cast<unsigned long long>(byPasses[1]),
                  static_cast<unsigned long long>(byPasses[3]), static_cast<unsigned long long>(byPasses[4]), sharedMs / n,
                  sharedDraws / n, passMs[0] / n, maxPassMs[0], passDraws[0] / n, passInstanced[0] / n, passMs[1] / n,
                  maxPassMs[1], passDraws[1] / n, passInstanced[1] / n, postMs / n, postDraws / n, ends_of(0).c_str(),
                  ends_of(1).c_str());
    return buf;
}

// ---- The D3D11 side ----

namespace {

using Microsoft::WRL::ComPtr;

struct State {
    ID3D11DeviceContext* context = nullptr;  // raw: the game's
    PassTracker tracker;
    PassSummary summary;
    std::atomic<uint64_t> endFrames{0};  // xrEndFrame calls (any thread)
    uint64_t seenEndFrames = 0;          // as the render thread last took them
    uint64_t frames = 0, detailed = 0;
    unsigned loggedWidth = 0, loggedHeight = 0;
    DWORD thread = 0;                   // the thread of the first bind
    uint64_t otherThreadBinds = 0, lists = 0;
    double freq = 0;
} g;

double now_s() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / g.freq;
}

void log_frame(const PassTracker::Frame& f) {
    char passes[512] = {};
    int at = 0;
    for (int p = 0; p < f.passes && at < static_cast<int>(sizeof(passes)) - 80; ++p)
        at += std::snprintf(passes + at, sizeof(passes) - at, " | pass %d %.2f ms %llu draws (%llu instanced), ended by %s",
                            p, f.pass[p].ms, static_cast<unsigned long long>(f.pass[p].draws),
                            static_cast<unsigned long long>(f.pass[p].instanced), PassTracker::end_name(f.end[p]));
    simqvfr::logf("scs pass probe: frame %llu: eye %ux%u, shared %.2f ms %llu draws%s | post+rest %.2f ms %llu draws",
                  static_cast<unsigned long long>(g.frames), f.eyeWidth, f.eyeHeight, f.shared.ms,
                  static_cast<unsigned long long>(f.shared.draws), passes, f.post.ms,
                  static_cast<unsigned long long>(f.post.draws));
}

// On the render thread: the frame xrEndFrame closed.
void take_frame_end(double t) {
    const uint64_t ends = g.endFrames.load(std::memory_order_acquire);
    if (ends == g.seenEndFrames)
        return;
    g.seenEndFrames = ends;
    const PassTracker::Frame f = g.tracker.end_frame(t);
    ++g.frames;
    if (f.eyeWidth != g.loggedWidth || f.eyeHeight != g.loggedHeight) {
        g.loggedWidth = f.eyeWidth;
        g.loggedHeight = f.eyeHeight;
        simqvfr::logf("scs pass probe: the eye passes' G-buffer is %ux%u", f.eyeWidth, f.eyeHeight);
    }
    if (f.passes > 0 && g.detailed < 4) {
        ++g.detailed;
        log_frame(f);
    }
    g.summary.add(f);
    if (g.summary.frames >= 900) {
        simqvfr::logf("scs pass probe: %s; binds on other threads %llu, command lists %llu", g.summary.line().c_str(),
                      static_cast<unsigned long long>(g.otherThreadBinds), static_cast<unsigned long long>(g.lists));
        g.summary = PassSummary{};
    }
    simqvfr::context_hooks::rescan();
}

PassTracker::Bind read_bind(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    PassTracker::Bind b;
    b.depth = dsv != nullptr;
    for (UINT i = 0; i < count && rtvs; ++i)
        b.targets += rtvs[i] ? 1 : 0;
    if (count > 0 && rtvs && rtvs[0]) {
        b.first = rtvs[0];
        ComPtr<ID3D11Resource> res;
        rtvs[0]->GetResource(&res);
        ComPtr<ID3D11Texture2D> tex;
        if (res && SUCCEEDED(res.As(&tex))) {
            D3D11_TEXTURE2D_DESC d;
            tex->GetDesc(&d);
            b.width = d.Width;
            b.height = d.Height;
        }
    }
    return b;
}

void on_targets(ID3D11DeviceContext*, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    const DWORD self = GetCurrentThreadId();
    if (g.thread && self != g.thread) {
        ++g.otherThreadBinds;
        return;
    }
    const PassTracker::Bind b = read_bind(count, rtvs, dsv);
    if (!g.thread) {  // not following a thread yet: the first G-buffer's names it
        if (!PassTracker::gbuffer_shape(b)) {
            ++g.otherThreadBinds;
            return;
        }
        g.thread = self;
        g.seenEndFrames = g.endFrames.load(std::memory_order_acquire);
        simqvfr::logf("scs pass probe: following thread %lu (its first G-buffer bind, %ux%u; %llu binds before it on "
                      "any thread)", static_cast<unsigned long>(self), b.width, b.height,
                      static_cast<unsigned long long>(g.otherThreadBinds));
        g.otherThreadBinds = 0;
    }
    const double t = now_s();
    take_frame_end(t);
    g.tracker.bind(b, t);
}

void on_draw(ID3D11DeviceContext*, simqvfr::context_hooks::Entry entry, UINT, UINT instances) {
    if (!g.thread || GetCurrentThreadId() != g.thread)
        return;
    const bool instanced = entry == simqvfr::context_hooks::kDrawIndexedInstanced ||
                           entry == simqvfr::context_hooks::kDrawInstanced || instances != 1;
    g.tracker.draw(instanced);
}

void on_execute(ID3D11DeviceContext*, ID3D11CommandList*) { ++g.lists; }

}  // namespace

bool pass_probe_install(ID3D11Device* device) {
    if (!device || g.context)
        return g.context != nullptr;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g.freq = static_cast<double>(f.QuadPart);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    namespace ch = simqvfr::context_hooks;
    ch::Observers o;
    o.targets = &on_targets;
    o.draw = &on_draw;
    o.execute = &on_execute;
    const uint32_t wanted = (1u << ch::kOMSetRenderTargets) | (1u << ch::kOMSetRenderTargetsAndUavs) |
                            (1u << ch::kDrawIndexed) | (1u << ch::kDraw) | (1u << ch::kDrawIndexedInstanced) |
                            (1u << ch::kDrawInstanced) | (1u << ch::kDrawIndexedInstancedIndirect) |
                            (1u << ch::kDrawInstancedIndirect) | (1u << ch::kExecuteCommandList);
    if (!ch::install(context.Get(), wanted, o)) {
        simqvfr::log_line("scs pass probe: the context couldn't be hooked: off");
        return false;
    }
    g.context = context.Get();
    simqvfr::log_line("scs pass probe: on (log only): eye passes found from the D3D11 render-target binds, timed on the "
                      "render thread; a summary every 900 frames");
    return true;
}

void pass_probe_frame_end() {
    if (g.context)
        g.endFrames.fetch_add(1, std::memory_order_release);
}

void pass_probe_session_end() {
    if (!g.context)
        return;
    simqvfr::context_hooks::uninstall();
    if (const std::string last = g.summary.line(); !last.empty())
        simqvfr::logf("scs pass probe: at session end: %s", last.c_str());
    g.context = nullptr;
}

}  // namespace scs
