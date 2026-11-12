// Live settings: re-reading sim-qvfr.ini while the game runs (on the ini worker), applying them, and
// writing the menu's changes back.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "cheeky/cheeky_host.h"
#include "cheeky/cheeky_settings.h"
#include "input/hotkeys.h"
#include "log.h"
#include "quad/focus_shape.h"
#include "quad/fov_crop.h"
#include "quad/helmet.h"
#include "quad/osd.h"
#include "quad/settings_menu.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

// Live resolution (views4): periphery_scale and focus_density resize the views inside ACE's target,
// up to their launch size (views4_live). A larger focus box at the same density needs more pixels;
// beyond the launch size the density gives.
void live_resolution(const LiveSettings& l) {
    const Views4Layout& launch = experiment.views4Layout;
    const Views4Sizes wanted = views4_sizes(live.ini.c_str(), l.focusFraction, l.focusFractionY);
    const double cw = l.crop.kept_width(), ch = l.crop.kept_height();  // the outer views take the live crop
    // One view size (X-Plane) or separate views (LMU): periphery and focus each sized on its own inside the launch
    // layers. Eye columns (ACC): each pair its own width within the launch column, the outer and focus rows sharing
    // the height.
    const bool columns = game->qvfr_eye_columns(),
               oneSize = (columns && game->qvfr_one_view_size()) || game->qvfr_separate_views();
    game->qvfr_set_live_layout(
        oneSize   ? views4_live_one_size(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch)
        : columns ? views4_live_eye_columns(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch)
                  : views4_live(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch));
    const Views4Fit fit = oneSize   ? views4_fit_one_size(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch)
                          : columns ? views4_fit_eye_columns(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch)
                                    : views4_fit(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch);
    logf("live: periphery %.0f%% of native, focus density %.0f%% x %.0f%% (focus views %.0f%% x %.0f%% of the native "
         "eye; launch %.0f%%, %.0f%% x %.0f%%)%s%s",
         fit.outer * 100, fit.focus / l.focusFraction * 100, fit.focusHeight / experiment.focus_y() * 100,
         fit.focus * 100, fit.focusHeight * 100, launch.outer * 100, launch.focus * 100, launch.focusHeight * 100,
         fit.focusCapped ? " (focus capped: larger than the launch target allows)" : "",
         fit.outerCapped ? (fit.outerByRoom ? " (periphery capped: the focus views use the width)"
                                            : " (periphery capped: larger than the launch target allows)")
                         : "");
}

// Live settings: applies the ini's live keys (LiveSettings). Called between display frames.
void live_apply(const LiveSettings& l) {
    if (experiment.composing()) {  // the helmet: either compositor
        std::lock_guard guard(quad.mutex);
        if (!(helmetSettings == l.helmet))
            logf("live: helmet %d, rim %d%%, corners %d%%, light %d%%, size %d deg, height %d deg, depth %d cm, visor %ls%s",
                 l.helmet.on ? 1 : 0, l.helmet.rim, l.helmet.corners, l.helmet.light, l.helmet.size, l.helmet.height,
                 l.helmet.depth,
                 kVisorPositionNames[static_cast<int>(l.helmet.visor)], l.helmet.debugLight ? ", showing its light" : "");
        helmetSettings = l.helmet;
    }
    if (experiment.views4) {
        const FovCrop before = experiment.crop();
        experiment.set_crop(l.crop);
        if (before.top != l.crop.top || before.bottom != l.crop.bottom || before.outer != l.crop.outer ||
            before.inner != l.crop.inner)
            logf("live: FOV crop top %.0f%%, bottom %.0f%%, outer %.0f%%, inner %.0f%%%s", l.crop.top * 100,
                 l.crop.bottom * 100, l.crop.outer * 100, l.crop.inner * 100,
                 game->qvfr_dlss_held() ? " (DLSS is on: the views keep their launch sizes until the next launch)" : "");
    }
    experiment.focusFraction = l.focusFraction;
    experiment.focusFractionY = l.focusFractionY;
    experiment.feather = l.feather;
    experiment.cornerRadius = l.cornerRadius;
    experiment.focusShape = focus_shape_bits(l.shape);
    std::memcpy(experiment.borderColor, l.borderColor, sizeof(experiment.borderColor));
    experiment.border = l.border;
    experiment.gaze = l.gaze;
    experiment.fixedOffsetY = l.offset.fixed;
    experiment.gazeOffsetY = l.offset.gaze;
    experiment.gazeDeadzone = l.gazeDeadzone;
    if (experiment.sharpen.exchange(l.sharpen) != l.sharpen)
        logf("live: sharpening %d%%%s", l.sharpen, l.sharpen ? " (CAS)" : " (off)");
    if (experiment.focusLayer == 2 && experiment.focusQuadDistance.exchange(l.focusQuadDistance) != l.focusQuadDistance)
        logf("live: focus_layer=2: the focus quads %d m out", l.focusQuadDistance);
    bool gazeAvailable;
    {
        std::lock_guard guard(gaze.mutex);
        gazeAvailable = gaze.set != XR_NULL_HANDLE;
    }
    if (experiment.views4 && game_features().objectDetail)
        game->qvfr_set_object_detail(l.objectDetail);  // the adapter logs a change
    if (experiment.views4 && !experiment.views4EyeCamera)
        game->qvfr_set_live(experiment.box_x(l.focusFraction),
                        experiment.box_y(l.focusFractionY > 0 ? l.focusFractionY : l.focusFraction),
                        1.0 + l.feather + experiment.views4FocusCullMargin,
                        l.gaze && experiment.composing());
    logf("live: focus box %.0f%% x %.0f%%, feather %.0f%%, corners %.0f%%, outline %d, gaze %d, offset fixed %d deg, "
         "gaze %d deg, gaze dead-zone %.1f deg",
         l.focusFraction * 100, experiment.focus_y() * 100, l.feather * 100, l.cornerRadius * 100, l.border ? 1 : 0,
         l.gaze ? 1 : 0, l.offset.fixed, l.offset.gaze, l.gazeDeadzone);
    logf("live: focus shape %ls", focus_shape_text(l.shape).c_str());
    if (experiment.views4 && game_features().liveResolution)
        live_resolution(l);
    else if (experiment.views4)
        logf("live: %ls resizes its views at the next launch (no live resolution yet)", game->info()->shortName);
    if (l.gaze && !gazeAvailable && (experiment.quad || experiment.composing()))
        log_line("live: gaze turned on, but eye tracking was off at launch; it starts at the next launch");
    if (l.border && !experiment.borderAtLaunch && experiment.views4 && !experiment.composing())
        log_line("live: debug_border without views4_compose starts at the next launch");
}

// Cheeky's runtime gets [cheeky]'s live settings and, in QVFR, the quad view roles (cheeky_settings.h).
void cheeky_apply_settings(const CheekySettings& settings, bool gazeOn) {
    if (!cheeky_running())
        return;
    CheekySettings c = settings;
    c.fg = c.fg && game_features().frameGeneration;  // a game without it: off, whatever the ini says
    cheeky_apply(cheeky_set_lines(c, experiment.views4, gazeOn, experiment.views4ComposeD3D11));
}

}  // namespace

void cheeky_apply_ini() {
    if (!live.ini.empty())
        cheeky_apply_settings(read_cheeky_settings(live.ini.c_str()), read_live(live.ini.c_str()).gaze);
}

namespace {

constexpr int64_t kLiveCheckMs = 250;

IniSnapshot read_snapshot(const wchar_t* ini) {
    IniSnapshot s;
    s.live = read_live(ini);
    s.cheeky = read_cheeky_settings(ini);
    s.menu = read_menu_values(ini);
    s.menuPlacement = read_menu_placement(ini);
    s.osd = read_osd_placement(ini);
    s.hotkeys = read_hotkeys(ini);
    s.restartKeys = restart_keys(ini, experiment.views4);
    return s;
}

void ini_worker_loop() {
    std::unique_lock lock(iniWorker.mutex);
    for (;;) {
        iniWorker.wake.wait_for(lock, std::chrono::milliseconds(kLiveCheckMs),
                                [] { return iniWorker.poke || !iniWorker.writes.empty(); });
        iniWorker.poke = false;
        std::deque<QueuedIniWrite> writes;
        writes.swap(iniWorker.writes);
        const std::wstring ini = iniWorker.ini;
        const FILETIME seen = iniWorker.written;
        const uint64_t pass = iniWorker.requested;
        // Numbered in order; a menu step that stood in for an older one skips that one's number.
        const uint64_t through = writes.empty() ? iniWorker.writesDone : writes.back().number;
        lock.unlock();
        for (auto& w : writes)
            w.write();
        // After its own writes it reads the ini whatever its write time says (a write can leave it unchanged).
        std::optional<IniSnapshot> snapshot;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        const bool stamped = !ini.empty() && GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &attributes);
        const bool changed = stamped && (!writes.empty() || CompareFileTime(&attributes.ftLastWriteTime, &seen) != 0);
        if (changed) {
            snapshot = read_snapshot(ini.c_str());
            snapshot->writesBefore = through;
        }
        lock.lock();
        iniWorker.writesDone = through;
        if (snapshot && ini == iniWorker.ini) {
            iniWorker.ready = std::move(snapshot);
            iniWorker.written = attributes.ftLastWriteTime;
        }
        iniWorker.done = std::max(iniWorker.done, pass);
        iniWorker.settled.notify_all();
    }
}

// Starts the worker (once) and points it at `ini`, read from `written` on (a new path drops what it read).
// Under iniWorker.mutex.
void ini_worker_watch(const std::wstring& ini, const FILETIME& written) {
    if (!iniWorker.started) {
        iniWorker.started = true;
        std::thread(ini_worker_loop).detach();  // ends with the process (ExitProcess stops it before teardown)
    }
    if (ini != iniWorker.ini) {
        iniWorker.ini = ini;
        iniWorker.written = written;
        iniWorker.ready.reset();
    }
}

}  // namespace

// Queues a write to the ini (or the launcher's) and wakes the worker; the live reload follows it. Returns
// the write's number: snapshots read after it carry writesBefore >= it. A menu step queued right behind another
// one not yet written replaces it: it writes every value the older one did (only the last in the queue, so a
// write of another kind between them keeps its order).
uint64_t ini_write_later(std::function<void()> write, IniWrite kind) {
    std::lock_guard guard(iniWorker.mutex);
    ini_worker_watch(live.ini, live.written);
    const uint64_t number = ++iniWorker.writesQueued;
    if (kind != IniWrite::Once && !iniWorker.writes.empty() && iniWorker.writes.back().kind == kind)
        iniWorker.writes.back() = {number, kind, std::move(write)};
    else
        iniWorker.writes.push_back({number, kind, std::move(write)});
    iniWorker.wake.notify_one();
    return number;
}

// Waits until the writes queued so far are done and the ini has been read after them (tests; the
// session's end, so no change is lost).
void ini_worker_sync() {
    std::unique_lock lock(iniWorker.mutex);
    if (!iniWorker.started)
        return;
    const uint64_t want = ++iniWorker.requested;
    iniWorker.poke = true;
    iniWorker.wake.notify_one();
    iniWorker.settled.wait(lock, [want] { return iniWorker.done >= want; });
}

// Applies the ini's live keys when the worker read a change. Only between display frames: with
// two_loops, never between frame A and frame B. No file access here.
void live_reload() {
    if (live.ini.empty())
        return;
    {
        std::lock_guard guard(iniWorker.mutex);
        ini_worker_watch(live.ini, live.written);
    }
    {
        std::lock_guard guard(loops.mutex);
        if (loops.active && loops.pairedEnds % 2 != 0)
            return;  // A ended, B not yet: apply after B
    }
    std::optional<IniSnapshot> s;
    {
        std::lock_guard guard(iniWorker.mutex);
        s.swap(iniWorker.ready);
    }
    if (!s)
        return;
    live_apply(s->live);
    cheeky_apply_settings(s->cheeky, s->live.gaze);
    if (experiment.views4 && game_features().sideLoadedDlss) {
        game->qvfr_set_dlss(s->menu.dlss);  // [dlss], read with the menu's values: the adapter applies a change
        game->qvfr_set_nr(s->menu.nr);      // [dlss_nr] likewise
    }
    menu_values_changed(*s);
    menu_placement_changed(s->menuPlacement);
    osd_placement_changed(s->osd, s->cheeky);
    hotkeys_changed(s->hotkeys);
    if (s->restartKeys != live.restartKeys) {
        logf("live: %ls-> %ls(applies at the next launch)", live.restartKeys.c_str(), s->restartKeys.c_str());
        live.restartKeys = s->restartKeys;
    }
}

}  // namespace simqvfr::layer
