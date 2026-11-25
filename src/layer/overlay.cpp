// The performance overlay and the session notice: measuring, placing, hotkeys and drawing them.
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <dxgi1_4.h>
#include <intrin.h>

#include "cheeky/cheeky_host.h"
#include "cheeky/cheeky_settings.h"
#include "engine/dlss_views.h"
#include "input/hotkeys.h"
#include "lang.h"
#include "log.h"
#include "probe/cpu_load.h"
#include "probe/d3d11_compose_timer.h"
#include "probe/gpu_frame_timer.h"
#include "probe/gpu_load.h"
#include "probe/hang_watch.h"
#include "quad/helmet.h"
#include "quad/menu_panel.h"
#include "quad/osd.h"
#include "quad/session_graphics.h"
#include "quad/settings_menu.h"
#include "quad/showcase.h"
#include "quad/qvfr_settings.h"
#include "ini_put.h"
#include "xr.h"
#include "layer/layer.h"
#include "layer/config.h"

namespace simqvfr::layer {

namespace {

constexpr int64_t kOsdDrawMs = 500, kOsdCheekyMs = 1000;
// The runtime's focus views held still this long: the card says so (a steady gaze still jitters them a little; on
// Pimax's runtime they moved on ~60% of frames while menus were looked at, 2026-11-11).
constexpr int64_t kOsdRuntimeGazeStillMs = 3000;

constexpr int64_t kNoticeMs = 30000;

constexpr OsdPlacement kNoticePlacement{true, 0, -6, 34};

// The showcase (quad/showcase.h): the overlay's card as the A/B table, its pixel rows from the menu's values of the
// latest live reload, and the Stock / QVFR hotkey. On the frame thread.
constexpr int kShowcaseVramFrames = 180;  // about 2 s at 90 Hz

struct Showcase {
    AbTable table;
    std::optional<MenuValues> values;  // the latest live reload's
    bool pixelsDirty = true;
    AbPixels pixels;
    std::wstring qvfrName = L"QVFR";  // with the multi-view badge QVFR last showed ("QVFR · MVR")
    bool hotkeyHeld = false;
    int activeLogged = -1;  // stock_active as last logged
    int vramLogIn = 0;      // frames until VRAM is logged again after a switch
    uint64_t sceneFrames = 0;  // scene_frames() at the last window
} showcase;

}  // namespace

void ab_values_changed(const MenuValues& values) {
    showcase.values = values;
    showcase.pixelsDirty = true;
}

void notice_set(std::wstring title, std::vector<std::wstring> lines) {
    notice.title = std::move(title);
    notice.lines = std::move(lines);
}

bool notice_pending() { return !notice.title.empty(); }

namespace {

constexpr size_t kOsdHistory = 150;

}  // namespace

void osd_waited(XrDuration period) { osd.periodNs.store(period, std::memory_order_relaxed); }

bool frame_generation_on() { return cheeky_running() && osd.cheeky.processing && osd.cheeky.fg; }

// Turbo's guard (layer/turbo.h): Cheeky's frame generation set to run (the ini's, kept by config.cpp and live_ini.cpp),
// or running.
bool cheeky_fg_set(const CheekySettings& c) { return game_features().frameGeneration && c.load && c.processing && c.fg; }

bool frame_generation_set() { return experiment.fgSet.load(std::memory_order_relaxed) || frame_generation_on(); }

int64_t menu_hide_ms() { return osd.placement.enabled ? kMenuHideOverlayMs : kMenuHideMs; }

void menu_frame_line() {
    bool bad = false;
    OsdSummary s;
    std::wstring line;
    if (osd_shown(s)) {
        line = osd_menu_line(s, bad);
    } else if (!osd.placement.enabled) {  // on, its first numbers not in yet: nothing
        const Hotkey& h = hotkeys[static_cast<int>(HotkeyAction::Overlay)];
        const std::wstring pad = h.pad.set() ? lang::trf(L"wheel {button}", {{L"button", control_text(h.pad.control)}})
                                             : std::wstring();
        const std::wstring key = h.key.set() ? key_text(h.key) : pad;
        // The headset menu's line beside its title, in the player's language (lang.h).
        line = key.empty() ? std::wstring(lang::tr(L"Turn the overlay on for frame times"))
                           : lang::trf(L"Overlay off: {key} for frame times", {{L"key", key}});
    }
    if (line != menu.model.frameLine || bad != menu.model.frameBad) {
        menu.model.frameLine = line;
        menu.model.frameBad = bad;
        menu.dirty = true;
    }
}

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

void osd_wait_held(double heldMs, int64_t heldTicks) {
    if (!osd.placement.enabled && !optimize.optimizer.running())
        return;
    // The hold moved from xrWaitFrame to xrBeginFrame: it is the frame's wait, not its work.
    osd.waitReturnQpc += heldTicks;
    osd.waitMs = heldMs;
}

void osd_measure(XrTime displayTime) {
    if (!osd.placement.enabled && !optimize.optimizer.running())
        return;
    // The refreshes since the previous frame, from the display times the runtime gave (the period: the steady one,
    // pacing_waited).
    const XrTime lastDisplay = std::exchange(osd.lastDisplay, displayTime);
    const int refreshes = osd_refreshes_between(lastDisplay, displayTime, osd.periodNs.load(std::memory_order_relaxed));
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
        frame.refreshes = refreshes;
        // A setting changed since the last frame (the session record's stints count them, live_ini.cpp): marked.
        static uint32_t seenGeneration = stintGeneration.load(std::memory_order_relaxed);
        const uint32_t generation = stintGeneration.load(std::memory_order_relaxed);
        frame.changed = generation != seenGeneration;
        seenGeneration = generation;
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
            osd.history.push_back(frame);
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

namespace {

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
        osd.lastDisplay = 0;
        osd.nextDraw = 0;
    }
    // The A/B table (quad/showcase.h): counted afresh each time it shows; its pixel rows want the menu's values, which
    // a write-less pass of the ini worker reads (the snapshot reaches ab_values_changed).
    const bool tableNow = placement.enabled && placement.abTable, tableBefore = osd.placement.enabled && osd.placement.abTable;
    if (tableNow && !tableBefore) {
        showcase.table.reset();
        showcase.sceneFrames = scene_frames();
        osd.nextDraw = 0;
        if (!showcase.values)
            ini_write_later([] {});
        logf("showcase: A/B table on (now %s)", stock_active() ? "Stock" : "QVFR");
    } else if (!tableNow && tableBefore) {
        log_line("showcase: A/B table off");
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

// The Stock / QVFR hotkey (Ctrl+Alt+S by default; quad/showcase.h): [quad] stock flipped in the ini the layer reloads
// (not the launcher's: each launch starts in QVFR), applied by the live reload as the menu's step is. Logs the game's
// side when it changes, whatever changed it.
void showcase_hotkey() {
    if (!game_features().showcase)
        return;
    const bool held = hotkey_held(HotkeyAction::ShowcaseStock);
    if (held && !showcase.hotkeyHeld) {
        const bool stock = !stock_wanted();
        ini_write_later([ini = live.ini, stock] { ini_put(ini.c_str(), kStock.section, kStock.key, stock ? 1 : 0); });
        logf("showcase: hotkey: %s", stock ? "Stock" : "QVFR");
    }
    showcase.hotkeyHeld = held;
    const int active = stock_active() ? 1 : 0;
    if (active != showcase.activeLogged) {
        if (showcase.activeLogged >= 0 || active)
            logf("showcase: the game renders %s", active ? "Stock" : "QVFR");
        if (showcase.activeLogged >= 0) {
            log_vram(active ? "the switch to Stock" : "the switch to QVFR");
            showcase.vramLogIn = kShowcaseVramFrames;  // and again once the other side settled
        }
        showcase.activeLogged = active;
    }
    if (showcase.vramLogIn > 0 && --showcase.vramLogIn == 0)
        log_vram(active ? "Stock, 2 s after the switch" : "QVFR, 2 s after the switch");
}

bool osd_resources(XrSession session) {
    if (!game->has_qvfr() && !game->shows_overlay())  // log only: nothing of ours in the headset
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
    l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied pixels (panel_rgba)
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
    // D3D11 (LMU, ACC, X-Plane): no queue timestamps, so the GPU tile is the whole GPU's load from Windows' engine
    // counters (probe/gpu_load.h), and the busiest thread from the thread sampler. xrCreateSession started both with
    // views4 or measure on (already running: nothing happens); they stop with the session.
    if (!graphics.d3d12 && graphics.adapter_luid(luid)) {
        gpu_load_start(static_cast<uint32_t>(luid.HighPart), luid.LowPart);
        cpu_load_start();
    }
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
        // A D3D11 game (LMU, ACC): no queue timestamps; the layer's composition timed by queries around it.
        if (experiment.views4ComposeD3D11) {
            int composed = 0;
            if (const double ms = d3d11_compose_timer_collect(composed); composed)
                in.composeMs = ms;
        }
    }
    if (osd.adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (SUCCEEDED(osd.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
            in.vramUsed = memory.CurrentUsage;
            in.vramBudget = memory.Budget;
        }
    }
    // The runtime composes quad views with its own foveated rendering: its eye tracking puts the focus views (and the
    // box) where the eyes look, and the layer may never ask the eye gaze extension (on Pimax's runtime it can even give
    // no samples while the runtime tracks: 2026-11-09). The card then says whether the runtime moves its focus views.
    const bool runtimeGaze = experiment.gaze && nativeQuad.active && nativeQuad.foveated;
    if (runtimeGaze) {
        uint64_t moves = 0;
        {
            std::lock_guard guard(nativeQuad.gazeMutex);
            moves = nativeQuad.gazeMoves;
        }
        if (moves != osd.runtimeGazeMoves || osd.runtimeGazeMoved == 0)  // moved (or the first card: not yet judged)
            osd.runtimeGazeMoved = now;
        osd.runtimeGazeMoves = moves;
        in.gaze = now - osd.runtimeGazeMoved < qpc_ms(kOsdRuntimeGazeStillMs) ? OsdGaze::Runtime : OsdGaze::RuntimeStill;
    } else {
        osd.runtimeGazeMoved = 0;
    }
    {
        std::lock_guard guard(gaze.mutex);
        const uint64_t queries = gaze.queries - osd.gazeQueries, tracked = gaze.tracked - osd.gazeTracked;
        osd.gazeQueries = gaze.queries;
        osd.gazeTracked = gaze.tracked;
        if (runtimeGaze) {
            // the runtime's eye tracking, above: these lookups (if any) don't steer the box
        } else if (!experiment.gaze || queries == 0) {  // off, or not asked (the focus does not follow the eyes here)
            in.gaze = gaze.action || !experiment.gaze ? OsdGaze::Off : OsdGaze::Unavailable;
        } else {
            in.gaze = tracked * 10 >= queries * 9 ? OsdGaze::Tracking : tracked * 2 >= queries ? OsdGaze::Partial : OsdGaze::Lost;
        }
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
    // Night car lighting's cost (LMU), while it runs.
    if (game) {
        double lightGpu = 0, lightCpu = 0;
        if (game->qvfr_car_light_costs(lightGpu, lightCpu)) {
            in.carLightGpuMs = lightGpu;
            in.carLightCpuMs = lightCpu;
        }
    }
    // The game's way of drawing its focus views (LMU: NVIDIA's multi-view rendering, the software one, or the fallback).
    if (game) {
        switch (game->qvfr_multi_view()) {
        case games::QvfrMultiView::none: in.multiView = OsdMultiView::None; break;
        case games::QvfrMultiView::nvidia: in.multiView = OsdMultiView::Mvr; break;
        case games::QvfrMultiView::software: in.multiView = OsdMultiView::Smvr; break;
        case games::QvfrMultiView::fallback: in.multiView = OsdMultiView::Fallback; break;
        case games::QvfrMultiView::eyesOnly: in.multiView = OsdMultiView::EyesOnly; break;
        }
    }
    if (!stock_active()) {  // QVFR's name in the A/B table, with its multi-view badge
        const std::wstring badge = osd_badge(in).value;
        const std::wstring mv = badge.substr(0, badge.find(L" + "));  // without the upscaler
        showcase.qvfrName = mv.empty() || in.multiView == OsdMultiView::EyesOnly || in.multiView == OsdMultiView::Fallback
                                ? std::wstring(L"QVFR")
                                : L"QVFR · " + mv;
    } else {
        in.multiView = OsdMultiView::Stock;  // the showcase (quad/showcase.h)
    }
    if (game) {  // The GPU regulator (RF2): the views' sizes it has brought the set ones down to
        const games::QvfrRegulatorStatus r = game->qvfr_regulator_status();
        in.regulatorOn = r.active;
        in.regulatorPeriphery = r.periphery;
        in.regulatorFocus = r.focus;
        in.regulatorPeripheryPercent = r.peripheryPercent;
        in.regulatorDensityPercent = r.densityPercent;
        in.regulatorCutPeriphery = r.cutPeriphery;
        in.regulatorCutDensity = r.cutDensity;
    }
    {  // DLSS: views evaluated since the card's last drawing
        static unsigned long long seen = 0;
        const unsigned long long total = dlss_evaluations_total();
        in.dlss = total != seen;
        seen = total;
        in.upscaler = dlss_upscaler_badge();  // DLSS, or AMD FSR's version where it runs instead
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
        // A timed-out wait: the image stays acquired (not released), waited on again at the next frame.
        if (acquire_wait(osd.chain, osd.pending, kImageWaitNs, 1, index) == XR_SUCCESS) {
            notice.uploaded = index < osd.images.size() && osd.upload && osd.upload->upload(osd.images[index], pixels);
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
    showcase_hotkey();
    if (!osd.placement.enabled || !osd_resources(session))
        return nullptr;
    if (now >= osd.nextDraw && !osd.drawing.valid()) {
        osd.nextDraw = now + qpc_ms(kOsdDrawMs);
        OsdInput in = osd_input(now);
        osd.shown = osd_summary(in);  // also for the log
        osd.shownValid = true;
        if (menu.visible)
            menu_frame_line();
        if (osd.placement.abTable) {  // the showcase's A/B table in the card's place (quad/showcase.h)
            const int64_t nowMs = now * 1000 / qpc_ms(1000);
            const bool stock = stock_active();
            std::vector<double> frameMs;
            frameMs.reserve(in.frames.size());
            for (const OsdFrame& f : in.frames)
                frameMs.push_back(f.intervalMs);
            const uint64_t scenes = scene_frames();
            showcase.table.add(stock, frameMs, osd.shown.cpuMs, osd.shown.gpuMs, nowMs, scenes - showcase.sceneFrames);
            showcase.sceneFrames = scenes;
            // The pixel rows: again after a reload, and until the native sharpness is known (after the first frames).
            if (showcase.values && (showcase.pixelsDirty || showcase.pixels.stockPpd <= 0)) {
                showcase.pixels = ab_pixels(*showcase.values);
                showcase.pixelsDirty = false;
            }
            const AbStats s = ab_stats(showcase.table.side(true)), q = ab_stats(showcase.table.side(false));
            const AbHeading heading{stock, showcase.table.settling(nowMs), showcase.qvfrName, s.seconds, q.seconds};
            osd.drawing = std::async(std::launch::async, [heading, lines = ab_lines(s, q, showcase.pixels)] {
                return draw_ab_panel(heading, lines);
            });
        } else {
            SYSTEMTIME local{};
            GetLocalTime(&local);  // the card's clock (redrawn every kOsdDrawMs: the minute turns over in time)
            osd.drawing = std::async(std::launch::async, [in = std::move(in), summary = osd.shown,
                                                          history = std::vector<OsdFrame>(osd.history.begin(), osd.history.end()),
                                                          clock = osd_clock(local.wHour, local.wMinute)] {
                return draw_osd_panel(summary, history, osd_tiles(in, summary), osd_rows(in, summary), osd_badge(in), clock);
            });
        }
    }
    if (osd.drawing.valid() && osd.drawing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::vector<uint32_t> pixels = osd.drawing.get();
        uint32_t index = 0;
        hang_watch_stage("overlay card: xrAcquireSwapchainImage");
        // A timed-out wait: the image stays acquired (not released), waited on again at the card's next upload; this
        // drawing is dropped (the next one comes kOsdDrawMs later).
        if (acquire_wait(osd.chain, osd.pending, kImageWaitNs, 1, index, "overlay card: xrWaitSwapchainImage") ==
            XR_SUCCESS) {
            bool ok = index < osd.images.size() && osd.upload;
            hang_watch_stage("overlay card: the upload");
            ok = ok && osd.upload->upload(osd.images[index], pixels);
            hang_watch_stage("overlay card: xrReleaseSwapchainImage");
            next.ReleaseSwapchainImage(osd.chain, nullptr);
            osd.uploaded = osd.uploaded || ok;
        }
        hang_watch_stage("overlay card: done");
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
    osd.pending = {};
    osd.uploaded = osd.failed = false;
    osd.frames.clear();
    osd.history.clear();
    osd.lastEnd = 0;
    osd.lastDisplay = 0;
}

// ---- Clock ---------------------------------------------------------------------------------------

namespace {

void clock_place() {
    const ClockPlacement& p = clockCard.placement;
    const MenuPose pose = menu_pose({p.yaw, p.pitch, p.width}, static_cast<float>(kClockHeight) / kClockWidth);
    clockCard.layer.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
    clockCard.layer.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    clockCard.layer.size = {pose.width, pose.height};
}

bool clock_resources(XrSession session) {
    if (!game->has_qvfr() && !game->shows_overlay())  // as the overlay's card: nothing of ours in the headset
        return false;
    if (clockCard.chain)
        return true;
    if (clockCard.failed)
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
    chainInfo.width = kClockWidth;
    chainInfo.height = kClockHeight;
    chainInfo.faceCount = 1;
    chainInfo.arraySize = 1;
    chainInfo.mipCount = 1;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &clockCard.view);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &clockCard.chain);
    if (XR_SUCCEEDED(r))
        r = swapchain_textures(graphics, next.EnumerateSwapchainImages, clockCard.chain, clockCard.images);
    if (XR_SUCCEEDED(r) && !clockCard.images.empty())
        clockCard.upload = make_panel_upload(graphics, kClockWidth, kClockHeight);
    if (XR_FAILED(r) || clockCard.images.empty() || !clockCard.upload) {
        logf("clock: card setup failed (%d, %s)", r, graphics.name());
        clockCard.failed = true;
        return false;
    }
    XrCompositionLayerQuad& l = clockCard.layer;
    l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied pixels (panel_rgba)
    l.space = clockCard.view;
    l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    l.subImage.swapchain = clockCard.chain;
    l.subImage.imageRect = {{0, 0}, {kClockWidth, kClockHeight}};
    clock_place();
    log_line("clock: card ready");
    return true;
}

}  // namespace

void clock_placement_changed(const ClockPlacement& placement) {
    if (clockCard.read && placement == clockCard.placement)
        return;
    if (clockCard.read || placement.enabled)
        logf("clock: %s, %d deg right, %d deg up, %d deg wide", placement.enabled ? "on" : "off", placement.yaw,
             placement.pitch, placement.width);
    clockCard.placement = placement;
    clockCard.read = true;
    clock_place();
}

const XrCompositionLayerBaseHeader* clock_frame(XrSession session) {
    if (!clockCard.read)
        clock_placement_changed(read_clock_placement(live.ini.c_str()));
    if (!clockCard.placement.enabled || !clock_resources(session))
        return nullptr;
    // Redrawn when the minute turns over: GetLocalTime is cheap, the drawing (GDI) goes to a worker.
    SYSTEMTIME local{};
    GetLocalTime(&local);
    std::wstring time = osd_clock(local.wHour, local.wMinute);
    if (time != clockCard.shown && !clockCard.drawing.valid()) {
        clockCard.shown = time;
        clockCard.drawing = std::async(std::launch::async, [time = std::move(time)] { return draw_clock_panel(time); });
    }
    if (clockCard.drawing.valid() && clockCard.drawing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::vector<uint32_t> pixels = clockCard.drawing.get();
        uint32_t index = 0;
        hang_watch_stage("clock card: xrAcquireSwapchainImage");
        // A timed-out wait: the image stays acquired, waited on again at the next upload; this drawing is dropped and
        // drawn again at the next frame.
        if (acquire_wait(clockCard.chain, clockCard.pending, kImageWaitNs, 1, index, "clock card: xrWaitSwapchainImage") ==
            XR_SUCCESS) {
            bool ok = index < clockCard.images.size() && clockCard.upload;
            hang_watch_stage("clock card: the upload");
            ok = ok && clockCard.upload->upload(clockCard.images[index], pixels);
            hang_watch_stage("clock card: xrReleaseSwapchainImage");
            next.ReleaseSwapchainImage(clockCard.chain, nullptr);
            clockCard.uploaded = clockCard.uploaded || ok;
            if (!ok)
                clockCard.shown.clear();
        } else {
            clockCard.shown.clear();
        }
        hang_watch_stage("clock card: done");
    }
    return clockCard.uploaded ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&clockCard.layer) : nullptr;
}

void clock_shutdown() {
    if (clockCard.drawing.valid())
        clockCard.drawing.wait();
    clockCard.drawing = {};
    clockCard.upload.reset();
    if (clockCard.chain)
        next.DestroySwapchain(clockCard.chain);
    if (clockCard.view)
        next.DestroySpace(clockCard.view);
    clockCard.chain = XR_NULL_HANDLE;
    clockCard.view = XR_NULL_HANDLE;
    clockCard.images.clear();
    clockCard.pending = {};
    clockCard.uploaded = clockCard.failed = false;
    clockCard.shown.clear();
}

}  // namespace simqvfr::layer
