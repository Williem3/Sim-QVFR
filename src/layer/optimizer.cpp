// The QVFR optimizer, the profile notice, and the frame and CPU capture hotkeys.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "input/hotkeys.h"
#include "log.h"
#include "probe/cpu_load.h"
#include "probe/cpu_profile.h"
#include "probe/d3d11_capture.h"
#include "probe/gpu_frame_timer.h"
#include "probe/gpu_load.h"
#include "quad/osd.h"
#include "quad/qvfr_optimizer.h"
#include "quad/settings_menu.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

constexpr int64_t kOptimizeWindowMs = 500;

// ---- QVFR optimizer --------------------------------------------------------------------------
// The settings menu's Optimize (quad/qvfr_optimizer.h): Right runs it (or stops a run), Left undoes it.
// Every kOptimizeWindowMs a window of ACE's frames goes to the optimizer: the GPU timer's own samples
// (90th percentile) and the render thread's work, which the overlay's rule (osd_summary) also uses to say
// whether the CPU limits the frame. The values it asks for are written as a menu step is, so the live
// reload resizes the views (or, with DLSS holding them, the next launch does). [optimize] in the ini:
// target_percent (of the frame budget), min_periphery, min_density, min_focus (the floors it lowers to).
void optimize_apply(const QvfrValues& q) {
    MenuValues& v = menu.model.values;
    v.periphery = q.periphery;
    v.density = q.density;
    v.width = q.width;
    v.height = q.height;
    menu.dirty = true;
    menu.lastWrite = ini_write_later([ini = live.ini, settings = menu.settingsIni, values = v] {
        write_menu_values(ini.c_str(), values, true);
        if (!settings.empty())
            write_menu_values(game_settings_ini(settings).c_str(), values, true);
    });
    logf("optimize: periphery %d%%, focus density %d%%, focus box %d%% x %d%%", q.periphery, q.density, q.width,
         q.height);
}

void optimize_start(int64_t now) {
    optimize.note.clear();
    // It measures what each step does, so the views must resize live (with DLSS: views4_dlss_live).
    if (game->qvfr_dlss_held()) {
        optimize.note = L"DLSS holds the view sizes until the next launch (DLAA, or views4_dlss_live=0): it can't measure live";
        return;
    }
    int nativeW = 0;
    {
        std::lock_guard guard(quad.mutex);
        if (!gpu_frame_timer_start(quad.device, quad.queue)) {
            optimize.note = L"No GPU frame timing on this device";
            return;
        }
        nativeW = static_cast<int>(quad.nativeW);
    }
    gpu_frame_timer_collect_optimizer();  // drop what the windows gathered before the run
    int samples = 0;
    gpu_load_window(samples, 2);
    cpu_load_window(samples, 2);
    optimize.frames.clear();
    optimize.nextWindow = now + qpc_ms(kOptimizeWindowMs);
    const wchar_t* ini = live.ini.c_str();
    const auto read = [ini](const wchar_t* key, int fallback, int lo, int hi) {
        return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"optimize", key, fallback, ini)), lo, hi);
    };
    const MenuValues& v = menu.model.values;
    const MenuLaunch& l = menu.model.launch;
    QvfrLimits limits;
    const int minFocus = read(L"min_focus", limits.floor.width, 10, 100);
    limits.floor = {read(L"min_periphery", limits.floor.periphery, 10, 100), read(L"min_density", limits.floor.density, 50, 400),
                    minFocus, minFocus};
    limits.cropWidth = v.crop_width();
    limits.cropHeight = v.crop_height();
    limits.nativeWidth = nativeW;
    // The views fit in the launch target (views4_live), at most their launch sizes.
    limits.ceiling.periphery = static_cast<int>(l.outer);
    limits.ceiling.density = static_cast<int>(std::min(l.focusWidth * 100 / v.width, l.focusHeight * 100 / v.height));
    if (nativeW > 0)
        limits.maxTargetWidth = static_cast<int>(2.0 * nativeW * (l.outer * l.cropWidth / 1e4 + l.focusWidth / 100));
    // From what is in effect (a value above the launch size renders at what fits).
    const MenuFit fit = menu_fit(menu.model);
    const QvfrValues current{static_cast<int>(std::lround(fit.periphery)), static_cast<int>(std::lround(fit.densityX)),
                             v.width, v.height};
    optimize.optimizer.start(current, limits, read(L"target_percent", 88, 50, 98) / 100.0);
    logf("optimize: start from periphery %d%%, focus density %d%%, box %d%% x %d%%; floors %d%%, %d%%, %d%%; "
         "ceilings %d%%, %d%%",
         current.periphery, current.density, current.width, current.height, limits.floor.periphery,
         limits.floor.density, limits.floor.width, limits.ceiling.periphery, limits.ceiling.density);
}

// The budget is the period xrWaitFrame gives ACE: with Cheeky's frame generation already the multiplied
// one, so `in` leaves frame generation off. The GPU time is the whole GPU's per ACE frame (frame
// generation, the composition and the runtime's compositor too) when the load sampler has it, else ACE's
// own queue (90th percentile).
QvfrWindow optimize_window() {
    const GpuFrameStats gpu = gpu_frame_timer_collect_optimizer();
    OsdInput in;
    in.frames.swap(optimize.frames);
    in.periodMs = static_cast<double>(osd.periodNs.load(std::memory_order_relaxed)) / 1e6;
    in.gpuMs = gpu.frames ? gpu.renderP90 : -1;
    int samples = 0;
    if (const GpuLoad load = gpu_load_window(samples, 2); load.valid) {
        in.gpuBusyPercent = load.busy;
        in.gpuGamePercent = load.game;
    }
    if (const CpuLoad threads = cpu_load_window(samples, 2); threads.valid) {
        in.cpuBusiestPercent = threads.busiestPercent;
        in.cpuBusiestIsRender = threads.busiestThread == osd.thread;
    }
    const OsdSummary s = osd_summary(in);
    QvfrWindow w;
    w.gpuMs = s.gpuMs;
    w.cpuMs = s.cpuMs;
    w.budgetMs = s.budgetMs;
    w.cpuBound = s.bound == OsdBound::Cpu;
    w.frames = gpu.frames;
    return w;
}

}  // namespace

void optimize_tick(int64_t now) {
    QvfrOptimizer& o = optimize.optimizer;
    if (const int request = std::exchange(menu.model.optimizeRequest, 0); request != 0 && experiment.views4) {
        optimize.shown = true;
        if (request > 0 && o.running()) {
            o.cancel();
            log_line("optimize: stopped");
        } else if (request > 0) {
            optimize_start(now);
        } else {
            o.cancel();
            optimize.note.clear();
            if (const QvfrAction back = o.undo(); back.apply)
                optimize_apply(back.values);
        }
    }
    if (o.running()) {
        if (menu.visible)  // shown until it is done
            menu.hideAt = std::max(menu.hideAt, now + qpc_ms(kMenuHideMs));
        if (now >= optimize.nextWindow) {
            optimize.nextWindow = now + qpc_ms(kOptimizeWindowMs);
            if (const QvfrAction a = o.feed(optimize_window()); a.apply)
                optimize_apply(a.values);
        }
    } else {
        optimize.frames.clear();
    }
    // The menu's line and detail.
    std::wstring line, detail;
    if (optimize.shown) {
        detail = optimize.note.empty() ? o.status() : optimize.note;
        switch (o.state()) {
        case QvfrState::Measuring:
        case QvfrState::Settling: line = L"Running…"; break;
        case QvfrState::Done: line = o.can_undo() ? L"Done (Left: undo)" : L"Done"; break;
        case QvfrState::Failed: line = L"Not possible"; break;
        default: line = optimize.note.empty() ? L"" : L"Not possible"; break;
        }
    }
    if (line != menu.model.optimizeStatus || detail != menu.model.optimizeDetail) {
        if (detail != menu.model.optimizeDetail && !detail.empty())
            logf("optimize: %ls", detail.c_str());
        menu.model.optimizeStatus = line;
        menu.model.optimizeDetail = detail;
        menu.dirty = true;
    }
}

namespace {

// cpu_profile=1: a capture of the busiest threads (and this one, the render thread) at cpu_profile_frame, and
// whenever Ctrl+Alt+C is pressed while ACE has focus and none is running.
void profile_notice(const std::wstring& text, int64_t ms) {
    if (!experiment.views4 && !experiment.quad)
        return;  // no settings menu to show it in (baseline): the log has it
    const int64_t now = qpc_now();
    menu.model.notice = text;
    menu.dirty = true;
    menu_open(now);
    menu.hideAt = now + qpc_ms(ms);
    profileNotice.until = menu.hideAt;
}

}  // namespace

// The frame capture hotkey (d3d11_capture_hotkey, probe/d3d11_capture.h): each press saves the next frame's swapchain
// images, the game's and the composed output: what the headset gets at that moment.
void frame_capture_poll() {
    if (!experiment.d3d11CaptureHotkey)
        return;
    const bool held = hotkey_held(HotkeyAction::FrameCapture);
    const bool pressed = held && !frameCaptureHotkeyHeld;
    frameCaptureHotkeyHeld = held;
    if (pressed && d3d11_capture_arm(endFrames + 1))
        profile_notice(L"Frame capture: saved to the logs folder (captures)", 2500);
}

void cpu_profile_poll() {
    if (!experiment.cpuProfile)
        return;
    const bool running = cpu_profile_running();
    if (profileNotice.wasRunning && !running)
        profile_notice(L"CPU capture done: the report is in the logs folder", 5000);
    profileNotice.wasRunning = running;
    if (profileNotice.until && qpc_now() >= profileNotice.until) {
        profileNotice.until = 0;
        menu.model.notice.clear();
        menu.dirty = true;
    }
    const bool held = hotkey_held(HotkeyAction::CpuCapture);
    const bool pressed = held && !cpuProfileHotkeyHeld;
    cpuProfileHotkeyHeld = held;
    // cpu_profile_frame=0 is no capture by itself, not one at the session's first frame.
    if (!pressed && (experiment.cpuProfileFrame == 0 || endFrames != experiment.cpuProfileFrame))
        return;
    if (cpu_profile_running()) {
        log_line("cpu profile: a capture is running already");
        return;
    }
    // The render thread (this one), ACE's game and physics threads by name (busy or not at this moment: a
    // capture started on a loading screen still gets them), then the busiest others.
    CpuProfileRequest request;
    request.seconds = experiment.cpuProfileSeconds;
    request.slowFrameMs = experiment.cpuProfileSlowMs;
    request.threads.push_back(GetCurrentThreadId());
    const std::vector<ThreadLoad> all = cpu_load_latest_threads(256);
    for (const ThreadLoad& t : all)
        if (t.name == L"GameThread" || t.name == L"Physics")
            request.threads.push_back(t.id);
    for (size_t i = 0, added = 0; i < all.size() && added < static_cast<size_t>(experiment.cpuProfileThreads); ++i)
        if (std::find(request.threads.begin(), request.threads.end(), all[i].id) == request.threads.end()) {
            request.threads.push_back(all[i].id);
            ++added;
        }
    if (cpu_profile_start(request)) {
        profileNotice.wasRunning = true;
        profile_notice(L"CPU capture: " + std::to_wstring(experiment.cpuProfileSeconds) + L" s, keep driving", 4000);
    }
}

}  // namespace simqvfr::layer
