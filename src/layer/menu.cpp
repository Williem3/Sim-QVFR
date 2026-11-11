// The in-headset settings menu: its keys and controllers, profiles, rendering and frame layer.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <dxgi1_4.h>

#include "cheeky/cheeky_host.h"
#include "cheeky/cheeky_settings.h"
#include "engine/dlss_settings.h"
#include "engine/ngx_dlss.h"
#include "games/adapter.h"
#include "games/game.h"
#include "ini_put.h"
#include "input/hotkeys.h"
#include "log.h"
#include "profile/profiles.h"
#include "quad/focus_turn.h"
#include "quad/menu_panel.h"
#include "quad/osd.h"
#include "quad/session_graphics.h"
#include "quad/settings_menu.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

constexpr MenuKey kMenuKeyOf[] = {MenuKey::Up, MenuKey::Down, MenuKey::Left, MenuKey::Right};

}  // namespace

int64_t qpc_ms(int64_t ms) {
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency * ms / 1000;
}

bool game_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

namespace {

double native_center_ppd();  // below

void menu_place(const MenuPlacement& placement) {
    const MenuPose pose = menu_pose(placement, static_cast<float>(kPanelHeight) / kPanelWidth);
    menu.layer.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
    menu.layer.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    menu.layer.size = {pose.width, pose.height};
}

}  // namespace

// The launcher changed the ini: an open menu shows the new values.
// A snapshot read before the latest step was written would put the menu back a step (and the next step's
// write would carry the old value: two quick steps lost the second), so the menu waits for a newer one.
void menu_values_changed(const IniSnapshot& s) {
    if (!menu.visible || s.writesBefore < menu.lastWrite)
        return;
    menu.model.values = s.menu;
    menu.model.cheeky.values = s.cheeky;
    menu.dirty = true;
}

// Moved from the launcher (its Headset menu pane): the panel goes there and shows, so you can see it.
void menu_placement_changed(const MenuPlacement& placement) {
    if (placement == live.menuPlacement)
        return;
    live.menuPlacement = placement;
    menu_place(placement);
    logf("settings menu: moved to %d deg right, %d deg up, %d deg wide", placement.yaw, placement.pitch, placement.width);
    if (experiment.views4 || experiment.quad)
        menu_open(qpc_now());
}

namespace {

// Profiles (profile/profiles.h) in the launcher's folder (SIM_QVFR_SETTINGS), else next to the ini the layer reads.
std::wstring menu_profiles_ini() { return menu.settingsIni.empty() ? live.ini : menu.settingsIni; }

}  // namespace

// Where this game's per-game keys go in the launcher's folder (profile/profiles.h game_settings_target): the
// launcher's ini while it shows this game, else this game's own copy, so a change made in the headset never lands in
// the settings of the game the launcher shows. Reads the launcher's ini: on the ini worker.
std::wstring game_settings_ini(const std::wstring& settings) {
    const games::GameInfo* g = games::current_game();
    if (settings.empty() || !g)
        return settings;
    return game_settings_target(settings, std::wstring(g->id, g->id + std::strlen(g->id)));
}

namespace {

void menu_profiles_read() {
    const std::wstring ini = menu_profiles_ini();
    menu.model.profiles = profile_names(profiles_folder(ini));
    // The profile in use from the ini this game runs with: the launcher's may be showing another game's.
    const std::wstring current = current_profile((live.ini.empty() ? ini : live.ini).c_str());
    menu.model.profile = -1;
    for (size_t i = 0; i < menu.model.profiles.size(); ++i)
        if (_wcsicmp(menu.model.profiles[i].c_str(), current.c_str()) == 0)
            menu.model.profile = static_cast<int>(i);
    Profile p;
    menu.model.profileChanged = menu.model.profile >= 0 && load_profile(profiles_folder(ini), current, p, menu.model.values.eyeTracking) &&
                                !profile_matches(live.ini.c_str(), p);
}

// The profile the Profile line stepped to: its keys written to the ini the layer reloads and the launcher's, as
// the menu's own steps are; the menu shows its values once the live reload has them.
void menu_profile_use() {
    const int request = std::exchange(menu.model.profileRequest, -1);
    if (request < 0 || request >= static_cast<int>(menu.model.profiles.size()))
        return;
    const std::wstring ini = menu_profiles_ini();
    Profile p;
    if (!load_profile(profiles_folder(ini), menu.model.profiles[request], p, menu.model.values.eyeTracking)) {
        logf("settings menu: profile %ls not found", menu.model.profiles[request].c_str());
        return;
    }
    if (optimize.optimizer.running()) {
        optimize.optimizer.cancel();
        log_line("optimize: stopped (a profile was used)");
    }
    menu.lastWrite = ini_write_later([live = live.ini, settings = menu.settingsIni, p] {
        apply_profile(live.c_str(), p);
        set_current_profile(live.c_str(), p.name);
        if (!settings.empty()) {
            const std::wstring target = game_settings_ini(settings);
            apply_profile(target.c_str(), p);
            set_current_profile(target.c_str(), p.name);
        }
    });
    logf("settings menu: profile %ls (%zu keys)", p.name.c_str(), p.keys.size());
}

}  // namespace

void menu_open(int64_t now) {
    if (menu.settingsIni.empty()) {  // the launcher's own ini, where the next launch reads them
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", path, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            menu.settingsIni = path;
    }
    if (!menu.visible) {
        bool stepPending;  // reopened before its last step reached the ini: keep the values it shows
        {
            std::lock_guard guard(iniWorker.mutex);
            stepPending = iniWorker.writesDone < menu.lastWrite;
        }
        if (!stepPending)
            menu.model.values = read_menu_values(live.ini.c_str());
        menu.model.cheeky = {cheeky_running(), cheeky_version(),
                             stepPending ? menu.model.cheeky.values : read_cheeky_settings(live.ini.c_str())};
        const Views4Layout& l = experiment.views4Layout;
        {
            std::lock_guard guard(quad.mutex);  // the native eye, for the notes' pixel sizes
            menu.model.launch = {experiment.views4, l.outer * 100, l.focus * 100, l.focusHeight * 100,
                                 static_cast<int>(quad.nativeW), static_cast<int>(quad.nativeH), 0,
                                 experiment.eyeScale * 100};
        }
        menu.model.launch.cropWidth = l.cropWidth * 100;  // the launch crop
        menu.model.launch.cropHeight = l.cropHeight * 100;
        const games::GameFeatures features = game_features();  // what the game has: the rest is left out
        menu.model.launch.cropPage = features.fovCrop;
        menu.model.launch.helmetPage = features.helmet;
        menu.model.launch.fgPage = features.frameGeneration;
        menu.model.launch.objectDetail = features.objectDetail;  // X-Plane only
        menu.model.launch.sharpening = features.sharpening;
        menu.model.launch.focusQuads = experiment.focusLayer == 2 && !nativeQuad.active;  // its test: the quads' distance
        menu.model.launch.runtimeComposes = nativeQuad.active;  // Pimax / Varjo: the box inside their focus region
        menu.model.launch.liveResolution = features.liveResolution;
        // Sim-QVFR's own DLSS: a game with it, in QVFR on the multi-view path (the room beside each eye), in a build
        // with the NGX SDK.
        menu.model.launch.ownDlss = features.sideLoadedDlss && experiment.views4 &&
                                    experiment.views4Layout.secondViewRoom && ngx::built_in();
        {
            std::lock_guard guard(quad.mutex);
            menu.model.launch.helmetPieces = quad.helmetPiecesLoaded;
        }
        menu.model.launch.nativePpd = native_center_ppd();  // takes quad.mutex itself
        if (!optimize.optimizer.running()) {  // a finished run's detail is not kept over a reopen
            optimize.shown = false;
            optimize.note.clear();
        }
        if (!stepPending)  // the profiles, and the one in use unless a step of the menu's is on its way
            menu_profiles_read();
        menu.visible = menu.dirty = true;
        log_line("settings menu: open");
    }
    menu.hideAt = now + qpc_ms(kMenuHideMs);
}

void menu_press(MenuKey key, int64_t now) {
    if (!menu.visible) {  // the first press only opens it
        menu_open(now);
        return;
    }
    menu.hideAt = now + qpc_ms(kMenuHideMs);
    menu.dirty = true;
    if (!menu_key(menu.model, key)) {
        menu_profile_use();
        return;
    }
    menu.model.profileChanged = menu.model.profile >= 0;
    if (optimize.optimizer.running()) {  // a value changed by hand: the optimizer's measurements no longer hold
        optimize.optimizer.cancel();
        log_line("optimize: stopped (a setting was changed)");
    }
    {  // what was set, to check against what applies ("live:", "cheeky: settings applied")
        const std::vector<MenuLine> lines = menu_lines(menu.model);
        const int i = menu.model.selected;
        if (i >= 0 && i < static_cast<int>(lines.size()))
            logf("settings menu: %ls: %ls -> %ls", menu_title(menu.model).c_str(), lines[i].label.c_str(),
                 lines[i].value.c_str());
    }
    // Written on the ini worker (off the render thread), to the ini the layer reloads and the launcher's
    // (the next launch starts from them); the live reload follows within a frame or two. Steps quicker than the
    // worker are written as one (IniWrite::MenuStep).
    menu.lastWrite = ini_write_later([ini = live.ini, settings = menu.settingsIni, values = menu.model.values,
                     cheeky = menu.model.cheeky, qvfr = menu.model.launch.qvfr, detail = menu.model.launch.objectDetail,
                     quads = menu.model.launch.focusQuads, sharpening = menu.model.launch.sharpening,
                     ownDlss = menu.model.launch.ownDlss] {
        write_menu_values(ini.c_str(), values, qvfr, detail, quads, sharpening);
        if (cheeky.available)
            write_cheeky_settings(ini.c_str(), cheeky.values, false);
        if (ownDlss)
            write_dlss_settings(ini.c_str(), values.dlss);
        if (!settings.empty()) {
            const std::wstring target = game_settings_ini(settings);
            write_menu_values(target.c_str(), values, qvfr, detail, quads, sharpening);
            if (cheeky.available)
                write_cheeky_settings(target.c_str(), cheeky.values, false);
            if (ownDlss)
                write_dlss_settings(target.c_str(), values.dlss);
            if (target != settings) {  // the panels' places and the overlay are every game's: the launcher's ini
                write_osd_placement(settings.c_str(), values.overlay);
                ini_put(settings.c_str(), L"menu", L"yaw", values.placement.yaw);
                ini_put(settings.c_str(), L"menu", L"pitch", values.placement.pitch);
                ini_put(settings.c_str(), L"menu", L"width", values.placement.width);
                ini_put(settings.c_str(), L"menu", L"hotkey_footer", values.hotkeyFooter ? 1 : 0);
            }
        }
    }, IniWrite::MenuStep);
}

namespace {

// Held keys and stick directions repeat after kRepeatDelayMs, every kRepeatMs.
bool repeat(int& held, int64_t& nextAt, int direction, int64_t now) {
    if (direction < 0) {
        held = -1;
        return false;
    }
    if (held != direction) {
        held = direction;
        nextAt = now + qpc_ms(kRepeatDelayMs);
        return true;
    }
    if (now < nextAt)
        return false;
    nextAt = now + qpc_ms(kRepeatMs);
    return true;
}

// The menu's hotkeys: the open/close one, and the four directions. A key's first press opens the menu; a wheel's
// directions act only while it shows (the game may use the same switch).
void menu_keyboard(int64_t now) {
    pad_watch_ensure();
    const bool toggle = hotkey_held(HotkeyAction::MenuToggle);
    if (toggle && !menu.hotkeyTogglePrevious) {
        if (menu.visible) {
            menu.visible = false;
            log_line("settings menu: closed");
        } else {
            menu_open(now);
        }
    }
    menu.hotkeyTogglePrevious = toggle;
    int down = -1;
    for (int i = 0; i < 4 && down < 0; ++i) {
        const HotkeyAction a = static_cast<HotkeyAction>(static_cast<int>(HotkeyAction::MenuUp) + i);
        if (key_held(hotkeys[static_cast<int>(a)].key) || (menu.visible && pad_held(a)))
            down = i;
    }
    if (repeat(menu.heldKey, menu.keyRepeat, down, now))
        menu_press(kMenuKeyOf[down], now);
}

void menu_controllers(XrSession session, int64_t now) {
    bool toggle = false;
    XrVector2f stick{};
    {
        std::lock_guard guard(gaze.mutex);
        if (!gaze.attached || !gaze.menuToggle)
            return;
        if (gaze.appSyncs == 0) {  // ACE never syncs actions: sync the layer's set alone
            const XrActiveActionSet active{gaze.set, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            next.SyncActions(session, &sync);
        }
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = gaze.menuToggle;
        XrActionStateBoolean pressed{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(next.GetActionStateBoolean(session, &get, &pressed)) && pressed.isActive)
            toggle = pressed.currentState;
        get.action = gaze.menuNavigate;
        XrActionStateVector2f axes{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(next.GetActionStateVector2f(session, &get, &axes)) && axes.isActive)
            stick = axes.currentState;
        if (!menu.profileLogged && (toggle || stick.x != 0 || stick.y != 0)) {
            menu.profileLogged = true;
            XrPath hand = XR_NULL_PATH;
            XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
            const bool known = XR_SUCCEEDED(next.StringToPath(gaze.instance, "/user/hand/right", &hand)) &&
                               XR_SUCCEEDED(next.GetCurrentInteractionProfile(session, hand, &state));
            logf("settings menu: controller input from %s",
                 known ? path_name(gaze.instance, state.interactionProfile).c_str() : "?");
        }
    }
    if (toggle && !menu.togglePrevious) {
        if (menu.visible) {
            menu.visible = false;
            log_line("settings menu: closed");
        } else {
            menu_open(now);
        }
    }
    menu.togglePrevious = toggle;
    // A clear push in one direction; back below kRelease ends it.
    constexpr float kPush = 0.6f, kRelease = 0.3f;
    int dir = menu.stickDir;
    const float ax = std::fabs(stick.x), ay = std::fabs(stick.y);
    if (std::max(ax, ay) < kRelease)
        dir = -1;
    else if (ay >= kPush && ay > ax)
        dir = stick.y > 0 ? 0 : 1;
    else if (ax >= kPush && ax > ay)
        dir = stick.x < 0 ? 2 : 3;
    if (menu.visible && repeat(menu.stickDir, menu.stickRepeat, dir, now))
        menu_press(kMenuKeyOf[dir], now);
    else if (!menu.visible)
        menu.stickDir = dir;  // a push held while opening does not step
}

bool menu_resources(XrSession session) {
    if (!game->has_qvfr())  // log only: nothing of ours in the headset
        return false;
    if (menu.chain)
        return true;
    if (menu.failed)
        return false;
    const SessionGraphics graphics = session_graphics_now();
    if (!graphics.valid())
        return false;  // no D3D11 or D3D12 session (yet)
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    chainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    chainInfo.sampleCount = 1;
    chainInfo.width = kPanelWidth;
    chainInfo.height = kPanelHeight;
    chainInfo.faceCount = 1;
    chainInfo.arraySize = 1;
    chainInfo.mipCount = 1;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &menu.view);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &menu.chain);
    if (XR_SUCCEEDED(r))
        r = swapchain_textures(graphics, next.EnumerateSwapchainImages, menu.chain, menu.images);
    if (XR_SUCCEEDED(r) && !menu.images.empty())
        menu.upload = make_panel_upload(graphics);
    if (XR_FAILED(r) || menu.images.empty() || !menu.upload) {
        logf("settings menu: panel setup failed (%d, %s); the menu still changes settings", r, graphics.name());
        menu.failed = true;
        return false;
    }
    XrCompositionLayerQuad& l = menu.layer;
    l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    l.space = menu.view;
    l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    l.subImage.swapchain = menu.chain;
    l.subImage.imageRect = {{0, 0}, {kPanelWidth, kPanelHeight}};
    const MenuPlacement& placement = live.menuPlacement;
    menu_place(placement);
    logf("settings menu: panel ready, %d deg right, %d deg up, %d deg wide", placement.yaw, placement.pitch, placement.width);
    return true;
}

// Pixels per degree at the centre of the view at the runtime's recommended size (100% density), over
// the FOV ACE renders with: views4 reads it back from ACE's projection (the focus density is against
// it), Two-loop uses frame A's. 0 until known.
double native_center_ppd() {
    float tan[4] = {};
    Views4Frame frame{};
    if (experiment.views4 && game->qvfr_latest_frame(frame))
        std::memcpy(tan, frame.outerTan[0], sizeof(tan));
    std::lock_guard guard(quad.mutex);
    if (!(tan[1] > tan[0])) {
        if (experiment.views4 && quad.v4FovValid)
            fov_tangents(quad.v4Fov[0], tan);
        else if (quad.aViewsValid)
            fov_tangents(quad.aViews[0].fov, tan);
    }
    // The native size spans the uncropped FOV (the crop keeps the density).
    const double width = (static_cast<double>(tan[1]) - tan[0]) / experiment.crop().kept_width();
    return quad.nativeW && width > 0.02 ? quad.nativeW / width * 3.14159265358979 / 180 : 0;
}

void menu_render() {
    const std::wstring title = menu_title(menu.model);
    const std::wstring hint = menu_hint();
    if (menu.model.launch.nativePpd <= 0)  // known once ACE has rendered a frame
        menu.model.launch.nativePpd = native_center_ppd();
    menu.model.launch.dlssHeld = experiment.views4 && game->qvfr_dlss_held();
    const std::vector<uint32_t> pixels = draw_menu_panel(title, menu_resolution(menu.model), menu_stats(menu.model),
                                                         menu_lines(menu.model),
                                                         menu_notes(menu.model), hint, menu_subtitle(menu.model),
                                                         menu.model.values.hotkeyFooter
                                                             ? hotkey_footer(menu.model.values.helmet.on && experiment.composing())
                                                             : L"");
    uint32_t index = 0;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100'000'000;
    if (XR_FAILED(next.AcquireSwapchainImage(menu.chain, nullptr, &index)))
        return;
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(menu.chain, &wait)) && index < menu.images.size() &&
                    menu.upload && menu.upload->upload(menu.images[index], pixels);
    next.ReleaseSwapchainImage(menu.chain, nullptr);
    if (ok) {
        menu.uploaded = true;
        menu.dirty = false;
    }
}

}  // namespace

// This frame's layers with the performance overlay and the menu panel on top while they are shown;
// `info` itself otherwise.
const XrFrameEndInfo* menu_frame(XrSession session, const XrFrameEndInfo* info, XrFrameEndInfo& storage,
                                 std::vector<const XrCompositionLayerBaseHeader*>& layers) {
    // The performance overlay also while measuring without QVFR (baseline=1), to compare the two; the settings
    // menu only with a method of the layer's (its settings would change nothing in mode=off).
    const bool method = experiment.views4 || experiment.quad;
    if (live.ini.empty() || (!method && !experiment.measure && !notice_pending()))
        return info;
    // For the launcher's Center PPD: once, when ACE has rendered.
    static bool ppdReported = false;
    if (method && !ppdReported && endFrames % 90 == 0) {
        if (const double ppd = native_center_ppd(); ppd > 0) {
            wchar_t text[32];
            swprintf_s(text, L"%.2f", ppd);
            runtime_info(L"native_center_ppd", text);
            logf("settings menu: %.2f px per degree at the centre at 100%% density", ppd);
            ppdReported = true;
        }
    }
    const int64_t now = qpc_now();
    if (method) {
        menu_keyboard(now);
        menu_controllers(session, now);
        optimize_tick(now);
    }
    if (menu.visible && now >= menu.hideAt) {
        menu.visible = false;
        log_line("settings menu: hidden");
    }
    bool showMenu = menu.visible && menu_resources(session);
    if (showMenu && menu.dirty)
        menu_render();
    showMenu = showMenu && menu.uploaded;
    const XrCompositionLayerBaseHeader* card = osd_frame(session, now);
    if (!showMenu && !card)
        return info;
    layers.assign(info->layers, info->layers + info->layerCount);
    if (card)
        layers.push_back(card);
    if (showMenu)  // the menu over the card
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu.layer));
    storage = *info;
    storage.layerCount = static_cast<uint32_t>(layers.size());
    storage.layers = layers.data();
    return &storage;
}

void menu_shutdown() {
    menu.upload.reset();
    if (menu.chain)
        next.DestroySwapchain(menu.chain);
    if (menu.view)
        next.DestroySpace(menu.view);
    menu.chain = XR_NULL_HANDLE;
    menu.view = XR_NULL_HANDLE;
    menu.images.clear();
    menu.visible = menu.uploaded = menu.failed = false;
    menu.dirty = true;
}

}  // namespace simqvfr::layer
