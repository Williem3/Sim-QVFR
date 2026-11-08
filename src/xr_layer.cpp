// XR_APILAYER_SIM_qvfr: logs what the game asks for (views, swapchains, frame submission) and starts the
// log-only engine probes (probe/probe.cpp) at xrCreateSession. With mode=off nothing alters rendering; the
// experiments and the quad pipeline are selected in sim-qvfr.ini. This file has the loader side:
// GetInstanceProcAddr, the layer order and gaze extension checks, CreateApiLayerInstance and the exported
// negotiation; the rest is in src/layer/ (layer.h).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <dxgi1_4.h>
#include <intrin.h>

#include "cheeky/cheeky_host.h"
#include "cheeky/cheeky_settings.h"
#include "games/adapter.h"
#include "games/game.h"
#include "input/hotkeys.h"
#include "layer_chain.h"
#include "log.h"
#include "probe/cpu_load.h"
#include "probe/cpu_profile.h"
#include "probe/d3d11_capture.h"
#include "probe/d3d12_trace.h"
#include "probe/dred.h"
#include "probe/gpu_frame_timer.h"
#include "probe/gpu_load.h"
#include "probe/hang_watch.h"
#include "probe/pass_census.h"
#include "probe/pix_capture.h"
#include "probe/probe.h"
#include "quad/helmet.h"
#include "quad/menu_panel.h"
#include "quad/osd.h"
#include "quad/qvfr_optimizer.h"
#include "quad/session_graphics.h"
#include "quad/settings_menu.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

constexpr int64_t kOptimizeWindowMs = 500;

constexpr int64_t kOsdDrawMs = 500, kOsdCheekyMs = 1000;

constexpr int64_t kNoticeMs = 30000;

constexpr OsdPlacement kNoticePlacement{true, 0, -6, 34};

void notice_set(std::wstring title, std::vector<std::wstring> lines) {
    notice.title = std::move(title);
    notice.lines = std::move(lines);
}

}  // namespace

bool notice_pending() { return !notice.title.empty(); }

namespace {

constexpr size_t kOsdHistory = 150;

}  // namespace

void osd_waited(XrDuration period) { osd.periodNs.store(period, std::memory_order_relaxed); }

bool frame_generation_on() { return cheeky_running() && osd.cheeky.processing && osd.cheeky.fg; }

bool osd_shown(OsdSummary& out) {
    if (!osd.placement.enabled || !osd.shownValid)
        return false;
    out = osd.shown;
    return true;
}

void osd_wait_returned(double waitMs) {
    if (!osd.placement.enabled && !optimize.optimizer.running())
        return;
    osd.waitReturnQpc = qpc_now();
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    osd.waitReturnCycles = cycles;
    osd.waitThread = GetCurrentThreadId();
    osd.waitMs = waitMs;
}

namespace {

void osd_measure() {
    if (!osd.placement.enabled && !optimize.optimizer.running())
        return;
    const int64_t now = qpc_now();
    const DWORD thread = GetCurrentThreadId();
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    if (!osd.tscStartQpc) {
        osd.tscStartQpc = now;
        osd.tscStart = __rdtsc();
    } else if (osd.cyclesPerMs == 0 && now - osd.tscStartQpc > qpc_ms(500)) {
        osd.cyclesPerMs = static_cast<double>(__rdtsc() - osd.tscStart) /
                          (static_cast<double>(now - osd.tscStartQpc) * 1000.0 / static_cast<double>(qpc_ms(1000)));
    }
    if (osd.lastEnd) {
        OsdFrame frame;
        frame.intervalMs = static_cast<double>(now - osd.lastEnd) * 1000.0 / static_cast<double>(qpc_ms(1000));
        if (thread == osd.waitThread && osd.waitReturnQpc > osd.lastEnd && osd.cyclesPerMs > 0) {
            // The render thread's work for the frame: from xrWaitFrame's return, its wall time and what of it ran.
            frame.workMs = static_cast<double>(now - osd.waitReturnQpc) * 1000.0 / static_cast<double>(qpc_ms(1000));
            frame.cpuMs = static_cast<double>(cycles - osd.waitReturnCycles) / osd.cyclesPerMs;
            frame.waitMs = osd.waitMs;
        } else if (thread == osd.thread && osd.cyclesPerMs > 0) {
            frame.cpuMs = static_cast<double>(cycles - osd.lastCycles) / osd.cyclesPerMs;
        }
        if (osd.placement.enabled) {
            osd.frames.push_back(frame);
            osd.history.push_back(frame.intervalMs);
            if (osd.history.size() > kOsdHistory)
                osd.history.pop_front();
        }
        if (optimize.optimizer.running())
            optimize.frames.push_back(frame);
    }
    osd.lastEnd = now;
    osd.thread = thread;
    osd.lastCycles = cycles;
}

void osd_place() {
    const OsdPlacement& p = notice_pending() ? kNoticePlacement : osd.placement;
    const MenuPose pose = menu_pose({p.yaw, p.pitch, p.width}, static_cast<float>(kOsdHeight) / kOsdWidth);
    osd.layer.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
    osd.layer.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    osd.layer.size = {pose.width, pose.height};
}

}  // namespace

void osd_placement_changed() {
    osd_placement_changed(read_osd_placement(live.ini.c_str()), read_cheeky_settings(live.ini.c_str()));
}

void osd_placement_changed(const OsdPlacement& placement, const CheekySettings& cheeky) {
    osd.cheeky = cheeky;  // frame generation on or off, its multiplier
    if (osd.read && placement == osd.placement)
        return;
    if (osd.read || placement.enabled)
        logf("performance overlay: %s, %d deg right, %d deg up, %d deg wide", placement.enabled ? "on" : "off",
             placement.yaw, placement.pitch, placement.width);
    if (placement.enabled && !osd.placement.enabled) {  // a fresh window
        osd.frames.clear();
        osd.history.clear();
        osd.lastEnd = 0;
        osd.nextDraw = 0;
    }
    osd.placement = placement;
    osd.read = true;
    osd_place();
}

namespace {

// The visor hotkey (Ctrl+Alt+V by default) steps the helmet's visor (closed, half, open, closed), written to the
// ini the layer reloads from and to the launcher's, as the headset menu writes it.
void visor_hotkey() {
    const bool held = hotkey_held(HotkeyAction::Visor);
    if (held && !visorHotkeyHeld && experiment.composing()) {
        HelmetSettings h;
        {
            std::lock_guard guard(quad.mutex);  // at once; the ini on the worker
            helmetSettings.visor = static_cast<VisorPosition>((static_cast<int>(helmetSettings.visor) + 1) % 3);
            h = helmetSettings;
        }
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", path, MAX_PATH);
        const std::wstring settings = n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
        ini_write_later([ini = live.ini, settings, h] {
            write_helmet_settings(ini.c_str(), h);
            if (!settings.empty())
                write_helmet_settings(game_settings_ini(settings).c_str(), h);
        });
        logf("helmet: visor %ls (hotkey)", kVisorPositionNames[static_cast<int>(h.visor)]);
    }
    visorHotkeyHeld = held;
}

void osd_hotkey() {
    pad_watch_ensure();  // also without a method of the layer's (baseline=1)
    const bool held = hotkey_held(HotkeyAction::Overlay);
    if (held && !osd.hotkeyHeld) {
        OsdPlacement placement = osd.placement;
        placement.enabled = !placement.enabled;
        wchar_t path[MAX_PATH] = {};  // the launcher's own ini: the next launch starts with it
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", path, MAX_PATH);
        const std::wstring settings = n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
        ini_write_later([ini = live.ini, settings, placement] {
            write_osd_placement(ini.c_str(), placement);
            if (!settings.empty())
                write_osd_placement(settings.c_str(), placement);
        });
        osd_placement_changed(placement, osd.cheeky);  // at once; the ini on the worker
    }
    osd.hotkeyHeld = held;
}

bool osd_resources(XrSession session) {
    if (!game->has_qvfr())  // log only: nothing of ours in the headset
        return false;
    if (osd.chain)
        return true;
    if (osd.failed)
        return false;
    const SessionGraphics graphics = session_graphics_now();
    if (!graphics.valid())
        return false;
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    chainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    chainInfo.sampleCount = 1;
    chainInfo.width = kOsdWidth;
    chainInfo.height = kOsdHeight;
    chainInfo.faceCount = 1;
    chainInfo.arraySize = 1;
    chainInfo.mipCount = 1;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &osd.view);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &osd.chain);
    if (XR_SUCCEEDED(r))
        r = swapchain_textures(graphics, next.EnumerateSwapchainImages, osd.chain, osd.images);
    if (XR_SUCCEEDED(r) && !osd.images.empty())
        osd.upload = make_panel_upload(graphics, kOsdWidth, kOsdHeight);
    if (XR_FAILED(r) || osd.images.empty() || !osd.upload) {
        logf("performance overlay: card setup failed (%d, %s)", r, graphics.name());
        osd.failed = true;
        return false;
    }
    XrCompositionLayerQuad& l = osd.layer;
    l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    l.space = osd.view;
    l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    l.subImage.swapchain = osd.chain;
    l.subImage.imageRect = {{0, 0}, {kOsdWidth, kOsdHeight}};
    osd_place();
    if (experiment.views4 && graphics.d3d12)  // the GPU frame timer reads D3D12 timestamps (D3D11: no GPU times)
        gpu_frame_timer_start(graphics.d3d12, graphics.queue);
    // VRAM: this process's use of the adapter's local memory against the budget Windows gives it.
    IDXGIFactory4* factory = nullptr;
    LUID luid{};
    if (graphics.adapter_luid(luid) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&osd.adapter))))
            osd.adapter = nullptr;
        factory->Release();
    }
    // D3D11 (ACC, X-Plane): no queue timestamps, so the GPU tile is the whole GPU's load from Windows' engine
    // counters (probe/gpu_load.h). A D3D12 session started it at xrCreateSession; it stops with the session.
    if (!graphics.d3d12 && graphics.adapter_luid(luid))
        gpu_load_start(static_cast<uint32_t>(luid.HighPart), luid.LowPart);
    log_line(osd.adapter ? "performance overlay: card ready" : "performance overlay: card ready (no VRAM numbers)");
    return true;
}

OsdInput osd_input(int64_t now) {
    OsdInput in;
    in.frames.swap(osd.frames);
    in.periodMs = static_cast<double>(osd.periodNs.load(std::memory_order_relaxed)) / 1e6;
    if (experiment.views4) {
        const GpuFrameStats gpu = gpu_frame_timer_collect();
        if (gpu.frames) {
            in.gpuMs = gpu.renderAvg;
            if (experiment.views4Compose)
                in.composeMs = gpu.composeAvg;
        }
    }
    if (osd.adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (SUCCEEDED(osd.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
            in.vramUsed = memory.CurrentUsage;
            in.vramBudget = memory.Budget;
        }
    }
    {
        std::lock_guard guard(gaze.mutex);
        const uint64_t queries = gaze.queries - osd.gazeQueries, tracked = gaze.tracked - osd.gazeTracked;
        osd.gazeQueries = gaze.queries;
        osd.gazeTracked = gaze.tracked;
        if (!experiment.gaze || queries == 0)  // off, or not asked (the focus does not follow the eyes here)
            in.gaze = gaze.action || !experiment.gaze ? OsdGaze::Off : OsdGaze::Unavailable;
        else
            in.gaze = tracked * 10 >= queries * 9 ? OsdGaze::Tracking : tracked * 2 >= queries ? OsdGaze::Partial : OsdGaze::Lost;
    }
    // Frame generation: Cheeky's, when the layer runs it with [cheeky] fg on.
    const CheekySettings& cheeky = osd.cheeky;
    if (cheeky_running() && cheeky.processing && cheeky.fg) {
        if (now >= osd.nextCheeky) {
            osd.cheekySnapshot = cheeky_snapshot();
            osd.nextCheeky = now + qpc_ms(kOsdCheekyMs);
        }
        const OsdCheeky c = osd_cheeky(osd.cheekySnapshot);
        in.fg = c.active ? OsdFg::On : OsdFg::Starting;
        in.fgMultiplier = cheeky.fgMultiplier;
        in.fgOutputFps = c.outputFps;
        in.fgMs = c.fgMs;
        in.fgState = c.state;
        in.gpuBusyPercent = c.gpuBusyPercent;
        if (c.refreshHz > 0)
            in.refreshMs = 1000.0 / c.refreshHz;  // the headset's own refresh (ACE's period may be multiplied)
    }
    // The whole GPU and ACE's threads, from the layer's own samplers (Cheeky's GPU share only without them),
    // averaged over the card's window (since its last draw).
    int samples = 0;
    if (const GpuLoad gpu = gpu_load_window(samples, 0); gpu.valid) {
        in.gpuBusyPercent = gpu.busy;
        in.gpuGamePercent = gpu.game;
    }
    if (const CpuLoad threads = cpu_load_window(samples, 0); threads.valid) {
        in.cpuBusiestPercent = threads.busiestPercent;
        in.cpuBusiestName = threads.busiestName;
        in.cpuBusiestIsRender = threads.busiestThread == osd.thread;
    }
    return in;
}

// The notice's card while it lasts (null once it is over, or if the card cannot be made); the overlay's card
// afterwards, redrawn before it shows again.
const XrCompositionLayerBaseHeader* notice_frame(XrSession session, int64_t now) {
    if (!notice_pending())
        return nullptr;
    if (!notice.until)
        notice.until = now + qpc_ms(kNoticeMs);
    if (now >= notice.until || !osd_resources(session)) {
        notice = {};
        osd.uploaded = false;  // the card holds the notice's pixels
        osd.nextDraw = 0;
        osd_place();
        log_line("notice: hidden");
        return nullptr;
    }
    if (!notice.uploaded) {
        osd_place();
        const std::vector<uint32_t> pixels = draw_notice_panel(notice.title, notice.lines);  // once: a few ms
        uint32_t index = 0;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000;
        if (XR_SUCCEEDED(next.AcquireSwapchainImage(osd.chain, nullptr, &index))) {
            notice.uploaded = XR_SUCCEEDED(next.WaitSwapchainImage(osd.chain, &wait)) && index < osd.images.size() &&
                              osd.upload && osd.upload->upload(osd.images[index], pixels);
            next.ReleaseSwapchainImage(osd.chain, nullptr);
        }
        if (notice.uploaded)
            log_line("notice: shown");
    }
    return notice.uploaded ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&osd.layer) : nullptr;
}

}  // namespace

const XrCompositionLayerBaseHeader* osd_frame(XrSession session, int64_t now) {
    if (const XrCompositionLayerBaseHeader* card = notice_frame(session, now))
        return card;
    if (!osd.read)
        osd_placement_changed();
    osd_hotkey();
    visor_hotkey();
    if (!osd.placement.enabled || !osd_resources(session))
        return nullptr;
    if (now >= osd.nextDraw && !osd.drawing.valid()) {
        osd.nextDraw = now + qpc_ms(kOsdDrawMs);
        OsdInput in = osd_input(now);
        osd.shown = osd_summary(in);  // also for the log
        osd.shownValid = true;
        osd.drawing = std::async(std::launch::async, [in = std::move(in), summary = osd.shown,
                                                      history = std::vector<double>(osd.history.begin(), osd.history.end())] {
            return draw_osd_panel(summary, history, osd_tiles(in, summary), osd_rows(in, summary));
        });
    }
    if (osd.drawing.valid() && osd.drawing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::vector<uint32_t> pixels = osd.drawing.get();
        uint32_t index = 0;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000;
        if (XR_SUCCEEDED(next.AcquireSwapchainImage(osd.chain, nullptr, &index))) {
            const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(osd.chain, &wait)) && index < osd.images.size() &&
                            osd.upload && osd.upload->upload(osd.images[index], pixels);
            next.ReleaseSwapchainImage(osd.chain, nullptr);
            osd.uploaded = osd.uploaded || ok;
        }
    }
    return osd.uploaded ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&osd.layer) : nullptr;
}

void osd_shutdown() {
    if (osd.drawing.valid())
        osd.drawing.wait();
    osd.drawing = {};
    osd.upload.reset();
    if (osd.chain)
        next.DestroySwapchain(osd.chain);
    if (osd.view)
        next.DestroySpace(osd.view);
    if (osd.adapter)
        osd.adapter->Release();
    osd.chain = XR_NULL_HANDLE;
    osd.view = XR_NULL_HANDLE;
    osd.adapter = nullptr;
    osd.images.clear();
    osd.uploaded = osd.failed = false;
    osd.frames.clear();
    osd.history.clear();
    osd.lastEnd = 0;
}

namespace {

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

}  // namespace

XrResult XRAPI_CALL layerEndFrame(XrSession session, const XrFrameEndInfo* info) {
    hang_watch_frame();
    cpu_profile_mark_frame();  // the game's frame ends here: a running capture's samples go to their frames
    cpu_load_render_thread(GetCurrentThreadId());
    cpu_profile_poll();
    frame_capture_poll();
    if ((experiment.views4 || experiment.measure) && cpu.msPerTick > 0) {
        const int64_t now = qpc_now();
        if (pacing.lastEnd && pacing.interval.size() < 4 * kCpuReportFrames)
            pacing.interval.push_back(static_cast<float>((now - pacing.lastEnd) * cpu.msPerTick));
        pacing.lastEnd = now;
    }
    osd_measure();   // before the layer's own work in the frame
    cheeky_frame();  // Cheeky's frame callback, after ACE's frame is recorded
    XrResult r;
    if (!cpu.realEndFrame) {
        r = end_frame(session, info);
    } else {
        const int64_t t0 = qpc_now();
        r = end_frame(session, info);
        cpu.endFrame.add(qpc_now() - t0);
        if (endFrames % kCpuReportFrames == 0)
            report_cpu_times();
    }
    live_reload();
    return r;
}

XrResult end_frame(XrSession session, const XrFrameEndInfo* frameInfo) {
    XrFrameEndInfo withMenu{};
    std::vector<const XrCompositionLayerBaseHeader*> menuLayers;
    const XrFrameEndInfo* info = menu_frame(session, frameInfo, withMenu, menuLayers);
    ++endFrames;
    if (experiment.pixCaptureFrame)
        pix_capture_frame(endFrames);
    if (sampled(endFrames)) {
        logf("xrEndFrame frame=%llu time=%lld layers=%u", static_cast<unsigned long long>(endFrames),
             static_cast<long long>(info->displayTime), info->layerCount);
        for (uint32_t i = 0; i < info->layerCount; ++i) {
            const XrCompositionLayerBaseHeader* layer = info->layers[i];
            if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                logf("  layer %u type=%d flags=0x%llx", i, layer->type,
                     static_cast<unsigned long long>(layer->layerFlags));
                continue;
            }
            auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            for (uint32_t v = 0; v < proj->viewCount; ++v) {
                const XrSwapchainSubImage& s = proj->views[v].subImage;
                const auto* chained = static_cast<const XrBaseInStructure*>(proj->views[v].next);
                logf("  projection view %u swapchain=%p rect=%d,%d %dx%d array=%u next=%d", v,
                     static_cast<void*>(s.swapchain), s.imageRect.offset.x, s.imageRect.offset.y,
                     s.imageRect.extent.width, s.imageRect.extent.height, s.imageArrayIndex,
                     chained ? chained->type : 0);
            }
        }
    }
    if (experiment.probes) {
        probe_frame_end();
        d3d12_trace_frame_end(probe_sequence_active());
    }
    if (experiment.passCensus)
        pass_census_frame_end();
    game->frame_end();
    if (experiment.views4) {
        game->qvfr_frame_end();
        return views4_submit(session, info);
    }
    if (!experiment.twoLoops)
        return next.EndFrame(session, info);

    std::unique_lock lock(loops.mutex);
    if (!loops.active) {
        ++loops.ends;
        if (loops.ends > loops.begins && !loops.broken) {
            loops.broken = true;
            log_line("two_loops: refused: xrEndFrame calls outnumber xrBeginFrame (a frame generator above this "
                     "layer, e.g. VectorXR Frame Generation?); turn it off for the game");
        }
        lock.unlock();
        return next.EndFrame(session, info);
    }
    LARGE_INTEGER now;
    const uint64_t index = loops.pairedEnds++;
    if (index % 2 == 0) {  // A: rendered for the display time, never submitted
        QueryPerformanceCounter(&now);
        loops.aMs += ms_between(loops.aWaitReturned, now);
        if (experiment.quad)
            quad_record_a(info);
        return XR_SUCCESS;
    }
    loops.inB = false;
    lock.unlock();
    const XrResult r = experiment.quad ? quad_submit_b(session, info) : next.EndFrame(session, info);
    lock.lock();
    // A fallback shows only frame B's focus region (black around it). Brief ones are harmless; a run of
    // them means composition does not work here, so stop pairing frames and let ACE render normally.
    if (experiment.quad) {
        std::lock_guard guard(quad.mutex);
        if (quad.fallbackRun >= kMaxFallbackRun) {
            loops.active = false;
            loops.broken = true;
            loops.inB = false;
            logf("quad: %llu frames in a row could not be composed; quad off, ACE runs unchanged",
                 static_cast<unsigned long long>(quad.fallbackRun));
        }
    }
    QueryPerformanceCounter(&now);
    loops.bMs += ms_between(loops.bWaitReturned, now);
    ++loops.pairs;
    const double sinceReport = ms_between(loops.lastReport, now);
    if (sinceReport >= 2000.0) {
        logf("two_loops: %.1f display frames/s, ACE frame A %.2f ms, frame B %.2f ms (wait return to end), "
             "result=%d, quad composed %llu fallbacks %llu, gaze tracked %llu/%llu (%.2f,%.2f,%.2f)",
             loops.pairs * 1000.0 / sinceReport, loops.aMs / loops.pairs, loops.bMs / loops.pairs, r,
             static_cast<unsigned long long>(quad.composed), static_cast<unsigned long long>(quad.fallbacks),
             static_cast<unsigned long long>(gaze.tracked), static_cast<unsigned long long>(gaze.queries),
             gaze.direction.x, gaze.direction.y, gaze.direction.z);
        loops.pairs = 0;
        loops.aMs = loops.bMs = 0;
        loops.lastReport = now;
    }
    return r;
}

#define SIMQVFR_INTERCEPT(fn)                                              \
    if (std::strcmp(name, "xr" #fn) == 0) {                              \
        *function = reinterpret_cast<PFN_xrVoidFunction>(&layer##fn);    \
        return XR_SUCCESS;                                               \
    }

XrResult XRAPI_CALL layerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function)
        return XR_ERROR_VALIDATION_FAILURE;
    SIMQVFR_INTERCEPT(GetInstanceProcAddr)
    if (next.EndFrame) {
        SIMQVFR_INTERCEPT(EnumerateViewConfigurationViews)
        SIMQVFR_INTERCEPT(CreateSession)
        SIMQVFR_INTERCEPT(DestroySession)
        SIMQVFR_INTERCEPT(BeginSession)
        SIMQVFR_INTERCEPT(WaitFrame)
        SIMQVFR_INTERCEPT(BeginFrame)
        SIMQVFR_INTERCEPT(CreateSwapchain)
        SIMQVFR_INTERCEPT(EnumerateSwapchainImages)
        SIMQVFR_INTERCEPT(LocateViews)
        SIMQVFR_INTERCEPT(CreateReferenceSpace)
        SIMQVFR_INTERCEPT(DestroySpace)
        SIMQVFR_INTERCEPT(EndFrame)
        SIMQVFR_INTERCEPT(AcquireSwapchainImage)
        SIMQVFR_INTERCEPT(ReleaseSwapchainImage)
        SIMQVFR_INTERCEPT(SuggestInteractionProfileBindings)
        SIMQVFR_INTERCEPT(AttachSessionActionSets)
        SIMQVFR_INTERCEPT(SyncActions)
        SIMQVFR_INTERCEPT(PollEvent)
        if (next.GetVisibilityMaskKHR)
            SIMQVFR_INTERCEPT(GetVisibilityMaskKHR)
    }
    return next.GetInstanceProcAddr ? next.GetInstanceProcAddr(instance, name, function) : XR_ERROR_HANDLE_INVALID;
}

namespace {

// The layers between this one and the runtime. VectorXR's frame generation has to be among them:
// above this layer it would get ACE's four-view image instead of the composed stereo frame.
void log_layer_order(const XrApiLayerNextInfo* below) {
    std::string names;
    bool vectorxrBelow = false;
    for (const XrApiLayerNextInfo* n = below; n; n = n->next) {
        names += names.empty() ? "" : ", ";
        names += n->layerName;
        vectorxrBelow |= std::strstr(n->layerName, "VECTORXR") != nullptr;
    }
    logf("layers below this one: [%s]", names.c_str());
    layersBelow = names;
    if (!vectorxrBelow && GetModuleHandleW(L"XR_APILAYER_DIENERTECH_VECTORXR.dll"))
        log_line("VectorXR is loaded above this layer: its frame generation would see the game's four-view image; move "
                 "this layer above VectorXR (machine-wide registration, then VectorXR's layer order)");
}

// Whether the layer adds XR_EXT_eye_gaze_interaction to the game's instance (layerCreateApiLayerInstance).
enum class GazeExtension {
    Requested,  // the game enables it itself
    Add,        // the runtime (through the layers below) lists it: add it
    Try,        // the list didn't answer: add it, and create the instance without it if that fails
    Absent,     // the runtime doesn't list it: leave the request as it is
};

GazeExtension gaze_extension(const XrInstanceCreateInfo& info, PFN_xrGetInstanceProcAddr gipa) {
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
        if (std::strcmp(info.enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            return GazeExtension::Requested;
    PFN_xrVoidFunction function = nullptr;
    if (!gipa || XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function)) || !function)
        return GazeExtension::Try;
    const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(function);
    uint32_t count = 0;
    if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr)))
        return GazeExtension::Try;
    std::vector<XrExtensionProperties> properties(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (count && XR_FAILED(enumerate(nullptr, count, &count, properties.data())))
        return GazeExtension::Try;
    for (const XrExtensionProperties& p : properties)
        if (std::strcmp(p.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            return GazeExtension::Add;
    return GazeExtension::Absent;
}

// Why a game can start flat with this layer loaded: the OpenXR loader negotiates every layer, then can still
// fail xrCreateInstance before the chain reaches this one (a layer named in XR_ENABLE_API_LAYERS whose manifest
// it refuses, an extension nobody provides). The loader prints its reason on the game's stderr, which the launcher
// keeps in the run folder's openxr-loader.log; this log names the layers and says when that happened.
void note_loader_negotiation() {
    const int n = ++loaderNegotiations;
    if (n == 1) {
        char enabled[2048] = {};
        GetEnvironmentVariableA("XR_ENABLE_API_LAYERS", enabled, static_cast<DWORD>(std::size(enabled)));
        logf("OpenXR layers named for this launch: %s", enabled[0] ? enabled : "(none: implicit layers only)");
    } else if (!instanceReachedLayer) {
        log_line("the game's previous xrCreateInstance failed before reaching this layer (no VR: the game starts "
                 "flat). The OpenXR loader's reason is in this launch's run folder, openxr-loader.log");
    }
}

// The layers named in SIM_QVFR_CHAIN, loaded from their manifests in XR_API_LAYER_PATH and linked below
// `self` (layer_chain.h: the launcher does this for a game whose OpenXR loader can't take a list of layers).
// A layer that fails to load is left out and logged, as the loader would. `self` when there is none.
const XrApiLayerNextInfo* chain_layers_below(const XrApiLayerNextInfo* self) {
    static std::vector<XrApiLayerNextInfo> storage;  // outlives the create call down the chain
    wchar_t value[4096] = {}, folder[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(chain::kChainVariable, value, static_cast<DWORD>(std::size(value)));
    if (n == 0 || n >= std::size(value))
        return self;
    GetEnvironmentVariableW(L"XR_API_LAYER_PATH", folder, static_cast<DWORD>(std::size(folder)));
    std::vector<chain::Layer> layers;
    char names[4096] = {};  // layer names are ASCII
    WideCharToMultiByte(CP_UTF8, 0, value, -1, names, static_cast<int>(std::size(names)), nullptr, nullptr);
    for (const std::string& name : chain::split_names(names)) {
        chain::Layer layer;
        std::string why;
        if (chain::load(folder, name, layer, why)) {
            layers.push_back(layer);
            logf("chain: %s loaded below this layer", name.c_str());
        } else {
            logf("chain: %s left out: %s", name.c_str(), why.c_str());
        }
    }
    return chain::link(layers, self, storage);
}

}  // namespace

XrResult XRAPI_CALL layerCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    if (!info || !layerInfo || !layerInfo->nextInfo || !instance ||
        std::strcmp(layerInfo->nextInfo->layerName, kLayerName) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;
    instanceReachedLayer = true;
    // This layer's node, with the layers the launcher asked it to chain (SIM_QVFR_CHAIN) linked below it.
    const XrApiLayerNextInfo* own = chain_layers_below(layerInfo->nextInfo);
    XrApiLayerCreateInfo nextLayerInfo = *layerInfo;
    nextLayerInfo.nextInfo = own->next;
    PFN_xrGetInstanceProcAddr gipa = own->nextGetInstanceProcAddr;
    // Registered as an implicit layer, so it loads into every OpenXR app: stay silent and
    // pass-through everywhere except a supported game (src/games).
    const games::GameInfo* thisGame = games::current_game();
    if (!thisGame) {
        next = Dispatch{gipa};
        return own->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    }
    // Eye tracking needs XR_EXT_eye_gaze_interaction on the instance. ACE and X-Plane ask for it themselves; ACC
    // (through OpenComposite) doesn't, so its gaze worked only with a layer below that adds it (Cheeky, OpenXR Toolkit,
    // VectorXR), and failed with none (the eye gaze profile's bindings rejected, XR_ERROR_PATH_UNSUPPORTED). So the
    // layer adds it when the game hasn't and the runtime offers it; with the offer unknown it tries, and creates the
    // instance without it if the runtime refuses.
    const GazeExtension gazeExtension = gaze_extension(*info, gipa);
    XrResult r = XR_ERROR_EXTENSION_NOT_PRESENT;
    if (gazeExtension == GazeExtension::Add || gazeExtension == GazeExtension::Try) {
        std::vector<const char*> names(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
        names.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        XrInstanceCreateInfo withGaze = *info;
        withGaze.enabledExtensionCount = static_cast<uint32_t>(names.size());
        withGaze.enabledExtensionNames = names.data();
        r = own->nextCreateApiLayerInstance(&withGaze, &nextLayerInfo, instance);
    }
    const bool gazeAdded = XR_SUCCEEDED(r);
    if (!gazeAdded)
        r = own->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    game = &games::adapter_for(thisGame);
    load_experiment();
    // Where the launcher writes live settings while this ACE runs (live_reload).
    runtime_info(L"live_ini", live.ini);
    runtime_info(L"live_pid", std::to_wstring(GetCurrentProcessId()));
    log_layer_order(own->next);
    logf("xrCreateApiLayerInstance app=%s engine=%s extensions=%u result=%d", info->applicationInfo.applicationName,
         info->applicationInfo.engineName, info->enabledExtensionCount, r);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        logf("  extension %s", info->enabledExtensionNames[i]);
    switch (gazeExtension) {
    case GazeExtension::Requested: log_line("gaze: the game enables XR_EXT_eye_gaze_interaction itself"); break;
    case GazeExtension::Absent: log_line("gaze: the runtime doesn't offer XR_EXT_eye_gaze_interaction (no eye tracking)"); break;
    case GazeExtension::Add:
    case GazeExtension::Try:
        logf("gaze: XR_EXT_eye_gaze_interaction %s (%s)",
             gazeAdded ? "added for the game" : "NOT added: the runtime refused the instance with it",
             gazeExtension == GazeExtension::Add ? "the runtime offers it" : "the runtime's offer unknown, tried");
        break;
    }
    if (XR_FAILED(r))
        return r;
    // Seconds before ACE initialises NGX, so Cheeky's hooks see its DLSS features created. Only in a game
    // it's made for.
    if (game->loads_cheeky()) {
        cheeky_start(cheeky_config(live.ini.c_str(), layer_directory()));
        {  // for the launcher's Foveated DLSS pane
            static const wchar_t* const kStatus[] = {L"off", L"missing", L"refused", L"running"};
            runtime_info(L"cheeky_status", kStatus[static_cast<int>(cheeky_status())]);
            if (cheeky_running())
                runtime_info(L"cheeky_version", cheeky_version());
        }
        cheeky_apply_ini();
    }
    // Before ACE creates its D3D12 device and renderer, so renderer init sees the patched mode.
    if (experiment.dred)
        dred_enable();
    if (experiment.d3d12Debug)
        d3d12_debug_enable(experiment.gpuValidation);
    if (experiment.pixCaptureFrame)  // before ACE creates its D3D12 device
        pix_capture_load(experiment.pixCaptureFrame);
    game->instance_created(live.ini.c_str(), experiment.mode);
    if (experiment.views4) {
        games::QvfrStart start;
        start.layout = experiment.views4Layout;
        start.focusFraction = experiment.views4EyeCamera ? 0.0 : experiment.box_x();
        start.focusFractionY = experiment.box_y();
        start.dlssLive = experiment.views4DlssLive;
        // The cull box: the focus box plus its feather band plus a gaze-latency allowance.
        start.focusCull = experiment.views4FocusCull;
        start.focusCullScale = 1.0 + experiment.feather + experiment.views4FocusCullMargin;
        start.focusCullGaze = experiment.gaze && experiment.composing();  // gaze turns only when composing
        start.ini = live.ini.c_str();
        if (!game->qvfr_start(start)) {
            experiment.views4 = false;  // no doubled eye width or rect rewrite without the engine side
            notice_set(L"Sim-QVFR is off",
                       {L"This " + std::wstring(thisGame->name) +
                            L" build isn’t one Sim-QVFR supports, so the game runs as normal, "
                            L"without foveated rendering. The game has probably been updated.",
                        L"Look for an Sim-QVFR release for the new build. The launcher’s Launch checks say the same "
                        L"before a launch."});
        }
    }
    Dispatch d;
    d.GetInstanceProcAddr = gipa;
    bool ok = true;
#define SIMQVFR_LOAD(fn) ok &= XR_SUCCEEDED(gipa(*instance, "xr" #fn, reinterpret_cast<PFN_xrVoidFunction*>(&d.fn))) && d.fn;
    SIMQVFR_LOAD(EnumerateViewConfigurationViews)
    SIMQVFR_LOAD(CreateSession)
    SIMQVFR_LOAD(DestroySession)
    SIMQVFR_LOAD(BeginSession)
    SIMQVFR_LOAD(WaitFrame)
    SIMQVFR_LOAD(BeginFrame)
    SIMQVFR_LOAD(CreateSwapchain)
    SIMQVFR_LOAD(EnumerateSwapchainImages)
    SIMQVFR_LOAD(LocateViews)
    SIMQVFR_LOAD(EndFrame)
    SIMQVFR_LOAD(AcquireSwapchainImage)
    SIMQVFR_LOAD(WaitSwapchainImage)
    SIMQVFR_LOAD(ReleaseSwapchainImage)
    SIMQVFR_LOAD(StringToPath)
    SIMQVFR_LOAD(CreateActionSet)
    SIMQVFR_LOAD(CreateAction)
    SIMQVFR_LOAD(SuggestInteractionProfileBindings)
    SIMQVFR_LOAD(AttachSessionActionSets)
    SIMQVFR_LOAD(SyncActions)
    SIMQVFR_LOAD(PollEvent)
    SIMQVFR_LOAD(CreateActionSpace)
    SIMQVFR_LOAD(LocateSpace)
    SIMQVFR_LOAD(CreateReferenceSpace)
    SIMQVFR_LOAD(DestroySpace)
    SIMQVFR_LOAD(DestroySwapchain)
    SIMQVFR_LOAD(GetActionStateBoolean)
    SIMQVFR_LOAD(GetActionStateVector2f)
    SIMQVFR_LOAD(GetActionStatePose)
    SIMQVFR_LOAD(GetCurrentInteractionProfile)
    SIMQVFR_LOAD(PathToString)
#undef SIMQVFR_LOAD
    gipa(*instance, "xrGetVisibilityMaskKHR", reinterpret_cast<PFN_xrVoidFunction*>(&d.GetVisibilityMaskKHR));
    if (ok && (experiment.views4 || experiment.measure)) {  // time what runs below this layer (report_cpu_times)
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        cpu.msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
        cpu.realEndFrame = d.EndFrame;
        cpu.realLocateViews = d.LocateViews;
        d.EndFrame = timedEndFrameBelow;
        d.LocateViews = timedLocateViewsBelow;
    }
    next = ok ? d : Dispatch{gipa};
    if (!ok)
        log_line("missing downstream functions; pass-through only");
    else
        gaze_init(*instance);
    return r;
}

}  // namespace simqvfr::layer

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
                                   XrNegotiateApiLayerRequest* request) {
    if (!loaderInfo || !layerName || !request || std::strcmp(layerName, simqvfr::layer::kLayerName) != 0 ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_API_VERSION_1_0;
    request->getInstanceProcAddr = simqvfr::layer::layerGetInstanceProcAddr;
    request->createApiLayerInstance = simqvfr::layer::layerCreateApiLayerInstance;
    if (simqvfr::layer::is_game_process()) {
        simqvfr::log_line("Loader negotiated API layer interface v1 (Sim-QVFR probe build)");
        simqvfr::layer::note_loader_negotiation();
    }
    return XR_SUCCESS;
}
