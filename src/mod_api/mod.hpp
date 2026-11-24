// The C++ side of a game mod (mod_api.h), header-only: derive from sqvfr::Mod, override what the game does, and
// export the table sqvfr::mod_table builds. Every method's default is GameAdapter's (src/games/adapter.h): a query
// answers kDefault (the core decides), everything else does nothing. Nothing here crosses the DLL boundary but the
// C table, so a mod may use any compiler and runtime library.
//
//   class MyGame final : public sqvfr::Mod { ... };
//   extern "C" __declspec(dllexport) const SqvfrMod* sqvfr_mod_entry(const SqvfrHost* host) {
//       if (!host || host->abi != SQVFR_ABI_VERSION) return nullptr;
//       static MyGame game(host);
//       static const SqvfrMod table = sqvfr::mod_table(game, "mygame");
//       return &table;
//   }
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "mod_api.h"

namespace sqvfr {

// The mod's own setting `key` (settings.json, set on the launcher's Game tab), as text: false when it isn't set (use
// your default) or the core is older than this service.
inline bool setting(const SqvfrHost* host, const char* key, std::string& value) {
    if (!host || host->size < offsetof(SqvfrHost, mod_setting) + sizeof(host->mod_setting) || !host->mod_setting)
        return false;
    char buffer[128] = {};
    if (host->mod_setting(host, key, buffer, sizeof(buffer)) < 0)
        return false;
    value = buffer;
    return true;
}
// A "toggle" setting: on or off, `def` when it isn't set.
inline bool setting_on(const SqvfrHost* host, const char* key, bool def) {
    std::string v;
    return setting(host, key, v) && (v == "1" || v == "0") ? v == "1" : def;
}
// A "choice" setting: the chosen option's value, `def` when it isn't set.
inline std::string setting_choice(const SqvfrHost* host, const char* key, const char* def) {
    std::string v;
    return setting(host, key, v) ? v : std::string(def);
}

class Mod {
public:
    // A query's "the core's default" (mod_api.h).
    static constexpr int32_t kDefault = -1;

    virtual ~Mod() = default;

    virtual int32_t has_qvfr() { return kDefault; }
    virtual void views_offered(uint32_t, uint32_t) {}
    virtual int32_t own_qvfr(const wchar_t*) { return kDefault; }
    virtual int32_t qvfr_composable() { return kDefault; }
    virtual int32_t qvfr_compose_d3d11() { return kDefault; }
    virtual int32_t loads_cheeky() { return kDefault; }
    virtual int32_t qvfr_eye_columns() { return kDefault; }
    virtual int32_t qvfr_one_view_size() { return kDefault; }
    virtual int32_t qvfr_separate_views() { return kDefault; }
    virtual bool qvfr_eye_extent(uint32_t (&)[2], uint32_t (&)[2]) { return false; }
    virtual void* qvfr_views_atlas(void* const (&)[2], float (&)[4][4]) { return nullptr; }
    virtual int32_t attaches_action_sets() { return kDefault; }
    virtual uint32_t hang_watch_s() { return 0; }

    // QVFR. qvfr_start's default refuses: a mod without QVFR leaves the game as it is.
    virtual bool qvfr_start(const SqvfrQvfrStart&) { return false; }
    virtual void qvfr_set_gaze(const float (&)[2][2], const int32_t (&)[2], bool) {}
    virtual void qvfr_set_live_layout(const SqvfrLayout&) {}
    virtual int32_t qvfr_live_resolution() { return kDefault; }
    virtual void qvfr_set_live(double, double, double, bool) {}
    virtual void qvfr_set_focus_mask(bool, const SqvfrFocusArea&, int32_t) {}
    virtual void qvfr_set_focus_visible(const float (&)[4]) {}
    virtual void qvfr_set_focus_corridor(bool, int32_t, int32_t, int32_t, int32_t, bool) {}
    virtual void qvfr_set_object_detail(int32_t) {}
    virtual void qvfr_set_dlss(const SqvfrDlssSettings&) {}
    virtual void qvfr_set_nr(const SqvfrNrSettings&) {}
    virtual bool qvfr_dlss_held() { return false; }
    virtual void qvfr_target_sizes(int32_t (&render)[2], int32_t (&output)[2]) {
        render[0] = render[1] = output[0] = output[1] = 0;
    }
    virtual bool qvfr_take_frame(SqvfrViews4Frame&) { return false; }
    virtual bool qvfr_latest_frame(SqvfrViews4Frame&) { return false; }
    virtual int32_t qvfr_copies_layout(const wchar_t*) { return kDefault; }
    virtual double qvfr_fixation_m() { return 0; }
    virtual int32_t qvfr_wants_game_image() { return kDefault; }
    virtual void qvfr_game_image(void*, const int32_t (&)[4][4]) {}
    virtual void qvfr_note_views(const SqvfrView*, uint32_t) {}
    virtual void qvfr_jitter_views(SqvfrView*, uint32_t) {}
    virtual void visibility_mask_taken(uint32_t) {}
    virtual void qvfr_frame_end() {}
    virtual int32_t qvfr_multi_view() { return SQVFR_MULTI_VIEW_NONE; }
    virtual bool qvfr_car_light_costs(double&, double&) { return false; }

    // The game's own experiments and watches.
    virtual void early_start(const wchar_t*) {}
    virtual void instance_created(const wchar_t*, const wchar_t*) {}
    virtual void session_created(bool) {}
    virtual void d3d11_session(void*) {}
    virtual int32_t observes_d3d11() { return kDefault; }
    virtual int32_t wants_gaze() { return kDefault; }
    virtual int32_t shows_overlay() { return kDefault; }
    virtual void live_view(const SqvfrLiveView&) {}
    virtual void qvfr_set_look(double, double, bool, const float (&)[4]) {}
    virtual void session_running(bool) {}
    virtual void session_destroyed() {}
    virtual void frame_end() {}
    virtual void swapchain_acquired(uint64_t, uint32_t, void*) {}
    virtual void swapchain_releasing(uint64_t) {}
    virtual void swapchain_destroyed(uint64_t) {}
    // Null: the frame as it was.
    virtual const void* adjust_frame_end(const void*, void*) { return nullptr; }
    // A D3D12 game whose mod renders the focus views itself: its table (focus_images_table), for the process.
    virtual const SqvfrFocusImages* focus_images() { return nullptr; }
};

// A D3D12 mod's own focus views (mod_api.h SqvfrFocusImages), the core composing them with the game's eyes: derive,
// override, and return the table focus_images_table builds from Mod::focus_images.
class FocusImages {
public:
    virtual ~FocusImages() = default;
    virtual void early(const SqvfrFocusImagesStart&) {}
    virtual bool start(const SqvfrFocusImagesStart&) { return false; }
    virtual void frame_end() {}
    virtual void set_gaze(const float (&)[2][2]) {}
    virtual void set_size(double, double, double) {}
    virtual void set_box(double, double) {}
    virtual void set_eye_scale(double) {}
    virtual void set_stock(bool) {}
    virtual void set_eye_texture_match(double, double) {}
    virtual bool focus_image(SqvfrFocusImage&) { return false; }
    virtual bool eye_region(float&, float&) { return false; }
    virtual void eye_allow(bool) {}
    virtual void gpu_scope(bool) {}
    virtual bool census(uint32_t) { return false; }
    virtual void set_live_diag(uint32_t) {}
    virtual void set_dlss_presets(int32_t, int32_t) {}
    virtual bool dlss_state(SqvfrFocusDlssState&) { return false; }
};

namespace detail {

inline Mod& as_mod(void* self) { return *static_cast<Mod*>(self); }
inline FocusImages& as_images(void* self) { return *static_cast<FocusImages*>(self); }
// A start from an older core, its missing fields zero.
inline SqvfrFocusImagesStart full_start(const SqvfrFocusImagesStart* s) {
    SqvfrFocusImagesStart full = {};
    if (s)
        std::memcpy(&full, s, s->size < sizeof(full) ? s->size : sizeof(full));
    full.size = sizeof(full);
    return full;
}
inline int32_t b(bool v) { return v ? 1 : 0; }

}  // namespace detail

// The C table for `mod` (which must outlive it), every entry a thunk to its virtual.
inline SqvfrMod mod_table(Mod& mod, const char* gameId) {
    using detail::as_mod;
    using detail::b;
    SqvfrMod t = {};
    t.size = sizeof(SqvfrMod);
    t.abi = SQVFR_ABI_VERSION;
    t.self = &mod;
    t.gameId = gameId;
    t.has_qvfr = [](void* s) { return as_mod(s).has_qvfr(); };
    t.views_offered = [](void* s, uint32_t w, uint32_t h) { as_mod(s).views_offered(w, h); };
    t.own_qvfr = [](void* s, const wchar_t* ini) { return as_mod(s).own_qvfr(ini); };
    t.qvfr_composable = [](void* s) { return as_mod(s).qvfr_composable(); };
    t.qvfr_compose_d3d11 = [](void* s) { return as_mod(s).qvfr_compose_d3d11(); };
    t.loads_cheeky = [](void* s) { return as_mod(s).loads_cheeky(); };
    t.qvfr_eye_columns = [](void* s) { return as_mod(s).qvfr_eye_columns(); };
    t.qvfr_one_view_size = [](void* s) { return as_mod(s).qvfr_one_view_size(); };
    t.qvfr_separate_views = [](void* s) { return as_mod(s).qvfr_separate_views(); };
    t.qvfr_eye_extent = [](void* s, uint32_t w[2], uint32_t h[2]) {
        return b(as_mod(s).qvfr_eye_extent(*reinterpret_cast<uint32_t(*)[2]>(w), *reinterpret_cast<uint32_t(*)[2]>(h)));
    };
    t.qvfr_views_atlas = [](void* s, void* const eyes[2], float uv[4][4]) {
        return as_mod(s).qvfr_views_atlas(*reinterpret_cast<void* const(*)[2]>(eyes),
                                          *reinterpret_cast<float(*)[4][4]>(uv));
    };
    t.attaches_action_sets = [](void* s) { return as_mod(s).attaches_action_sets(); };
    t.hang_watch_s = [](void* s) { return as_mod(s).hang_watch_s(); };

    t.qvfr_start = [](void* s, const SqvfrQvfrStart* start) { return b(start && as_mod(s).qvfr_start(*start)); };
    t.qvfr_set_gaze = [](void* s, const float tan[2][2], const int32_t valid[2], int32_t gazeSample) {
        as_mod(s).qvfr_set_gaze(*reinterpret_cast<const float(*)[2][2]>(tan),
                                *reinterpret_cast<const int32_t(*)[2]>(valid), gazeSample != 0);
    };
    t.qvfr_set_live_layout = [](void* s, const SqvfrLayout* live) {
        if (live)
            as_mod(s).qvfr_set_live_layout(*live);
    };
    t.qvfr_live_resolution = [](void* s) { return as_mod(s).qvfr_live_resolution(); };
    t.qvfr_set_live = [](void* s, double fx, double fy, double cull, int32_t gaze) {
        as_mod(s).qvfr_set_live(fx, fy, cull, gaze != 0);
    };
    t.qvfr_set_focus_mask = [](void* s, int32_t on, const SqvfrFocusArea* area, int32_t margin) {
        if (area)
            as_mod(s).qvfr_set_focus_mask(on != 0, *area, margin);
    };
    t.qvfr_set_focus_visible = [](void* s, const float visible[4]) {
        as_mod(s).qvfr_set_focus_visible(*reinterpret_cast<const float(*)[4]>(visible));
    };
    t.qvfr_set_focus_corridor = [](void* s, int32_t on, int32_t w, int32_t h, int32_t spread, int32_t blend,
                                   int32_t show) {
        as_mod(s).qvfr_set_focus_corridor(on != 0, w, h, spread, blend, show != 0);
    };
    t.qvfr_set_object_detail = [](void* s, int32_t percent) { as_mod(s).qvfr_set_object_detail(percent); };
    t.qvfr_set_dlss = [](void* s, const SqvfrDlssSettings* d) {
        if (d)
            as_mod(s).qvfr_set_dlss(*d);
    };
    t.qvfr_set_nr = [](void* s, const SqvfrNrSettings* n) {
        if (n)
            as_mod(s).qvfr_set_nr(*n);
    };
    t.qvfr_dlss_held = [](void* s) { return b(as_mod(s).qvfr_dlss_held()); };
    t.qvfr_target_sizes = [](void* s, int32_t render[2], int32_t output[2]) {
        as_mod(s).qvfr_target_sizes(*reinterpret_cast<int32_t(*)[2]>(render), *reinterpret_cast<int32_t(*)[2]>(output));
    };
    t.qvfr_take_frame = [](void* s, SqvfrViews4Frame* f) { return b(f && as_mod(s).qvfr_take_frame(*f)); };
    t.qvfr_latest_frame = [](void* s, SqvfrViews4Frame* f) { return b(f && as_mod(s).qvfr_latest_frame(*f)); };
    t.qvfr_copies_layout = [](void* s, const wchar_t* ini) { return as_mod(s).qvfr_copies_layout(ini); };
    t.qvfr_fixation_m = [](void* s) { return as_mod(s).qvfr_fixation_m(); };
    t.qvfr_wants_game_image = [](void* s) { return as_mod(s).qvfr_wants_game_image(); };
    t.qvfr_game_image = [](void* s, void* image, const int32_t rects[4][4]) {
        as_mod(s).qvfr_game_image(image, *reinterpret_cast<const int32_t(*)[4][4]>(rects));
    };
    t.qvfr_note_views = [](void* s, const SqvfrView* v, uint32_t n) { as_mod(s).qvfr_note_views(v, n); };
    t.qvfr_jitter_views = [](void* s, SqvfrView* v, uint32_t n) { as_mod(s).qvfr_jitter_views(v, n); };
    t.visibility_mask_taken = [](void* s, uint32_t view) { as_mod(s).visibility_mask_taken(view); };
    t.qvfr_frame_end = [](void* s) { as_mod(s).qvfr_frame_end(); };
    t.qvfr_multi_view = [](void* s) { return as_mod(s).qvfr_multi_view(); };
    t.qvfr_car_light_costs = [](void* s, double* gpu, double* cpu) {
        double g = 0, c = 0;
        const bool ok = as_mod(s).qvfr_car_light_costs(g, c);
        if (gpu)
            *gpu = g;
        if (cpu)
            *cpu = c;
        return b(ok);
    };

    t.early_start = [](void* s, const wchar_t* ini) { as_mod(s).early_start(ini); };
    t.instance_created = [](void* s, const wchar_t* ini, const wchar_t* mode) { as_mod(s).instance_created(ini, mode); };
    t.session_created = [](void* s, int32_t probed) { as_mod(s).session_created(probed != 0); };
    t.d3d11_session = [](void* s, void* device) { as_mod(s).d3d11_session(device); };
    t.observes_d3d11 = [](void* s) { return as_mod(s).observes_d3d11(); };
    t.wants_gaze = [](void* s) { return as_mod(s).wants_gaze(); };
    t.shows_overlay = [](void* s) { return as_mod(s).shows_overlay(); };
    t.live_view = [](void* s, const SqvfrLiveView* v) {
        if (v)
            as_mod(s).live_view(*v);
    };
    t.qvfr_set_look = [](void* s, double feather, double radius, int32_t border, const float color[4]) {
        as_mod(s).qvfr_set_look(feather, radius, border != 0, *reinterpret_cast<const float(*)[4]>(color));
    };
    t.session_running = [](void* s, int32_t running) { as_mod(s).session_running(running != 0); };
    t.session_destroyed = [](void* s) { as_mod(s).session_destroyed(); };
    t.frame_end = [](void* s) { as_mod(s).frame_end(); };
    t.swapchain_acquired = [](void* s, uint64_t chain, uint32_t index, void* enumerate) {
        as_mod(s).swapchain_acquired(chain, index, enumerate);
    };
    t.swapchain_releasing = [](void* s, uint64_t chain) { as_mod(s).swapchain_releasing(chain); };
    t.swapchain_destroyed = [](void* s, uint64_t chain) { as_mod(s).swapchain_destroyed(chain); };
    t.adjust_frame_end = [](void* s, const void* frameEnd, void* enumerate) {
        return as_mod(s).adjust_frame_end(frameEnd, enumerate);
    };
    t.focus_images = [](void* s) { return as_mod(s).focus_images(); };
    return t;
}

// The C table for `images` (which must outlive it), every entry a thunk to its virtual.
inline SqvfrFocusImages focus_images_table(FocusImages& images) {
    using detail::as_images;
    using detail::b;
    using detail::full_start;
    SqvfrFocusImages t = {};
    t.size = sizeof(SqvfrFocusImages);
    t.self = &images;
    t.early = [](void* s, const SqvfrFocusImagesStart* start) { as_images(s).early(full_start(start)); };
    t.start = [](void* s, const SqvfrFocusImagesStart* start) {
        return b(start && as_images(s).start(full_start(start)));
    };
    t.frame_end = [](void* s) { as_images(s).frame_end(); };
    t.set_gaze = [](void* s, const float ndc[2][2]) {
        as_images(s).set_gaze(*reinterpret_cast<const float(*)[2][2]>(ndc));
    };
    t.set_size = [](void* s, double k, double kY, double render) { as_images(s).set_size(k, kY, render); };
    t.set_box = [](void* s, double box, double boxY) { as_images(s).set_box(box, boxY); };
    t.set_eye_scale = [](void* s, double scale) { as_images(s).set_eye_scale(scale); };
    t.set_stock = [](void* s, int32_t on) { as_images(s).set_stock(on != 0); };
    t.set_eye_texture_match = [](void* s, double share, double mips) { as_images(s).set_eye_texture_match(share, mips); };
    t.focus_image = [](void* s, SqvfrFocusImage* out) { return b(out && as_images(s).focus_image(*out)); };
    t.eye_region = [](void* s, float* fx, float* fy) {
        float x = 1, y = 1;
        const bool ok = as_images(s).eye_region(x, y);
        if (fx)
            *fx = x;
        if (fy)
            *fy = y;
        return b(ok);
    };
    t.eye_allow = [](void* s, int32_t composed) { as_images(s).eye_allow(composed != 0); };
    t.gpu_scope = [](void* s, int32_t compositor) { as_images(s).gpu_scope(compositor != 0); };
    t.census = [](void* s, uint32_t frames) { return b(as_images(s).census(frames)); };
    t.set_live_diag = [](void* s, uint32_t bits) { as_images(s).set_live_diag(bits); };
    t.set_dlss_presets = [](void* s, int32_t eye, int32_t focus) { as_images(s).set_dlss_presets(eye, focus); };
    t.dlss_state = [](void* s, SqvfrFocusDlssState* out) { return b(out && as_images(s).dlss_state(*out)); };
    return t;
}

}  // namespace sqvfr
