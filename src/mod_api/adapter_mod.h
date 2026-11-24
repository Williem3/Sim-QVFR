// A first-party game's GameAdapter as a mod (mod.hpp), for the games still built into sim-qvfr.dll: every entry
// converts the ABI's structs back to the core's types and forwards to the adapter, and every query answers the
// adapter's own value (never the core's default), so the game behaves exactly as when the layer held its adapter
// directly. The step before a game moves into its own DLL (docs/architecture/mod-manager-plan.md, phase 1).
#pragma once

#include "../games/adapter.h"
#include "convert.h"
#include "mod.hpp"

namespace simqvfr::mod_api {

class AdapterMod final : public sqvfr::Mod {
public:
    explicit AdapterMod(games::GameAdapter& adapter) : a_(adapter) {}

    int32_t has_qvfr() override { return to_c(a_.has_qvfr()); }
    void views_offered(uint32_t w, uint32_t h) override { a_.views_offered(w, h); }
    int32_t own_qvfr(const wchar_t* ini) override { return to_c(a_.own_qvfr(ini)); }
    int32_t qvfr_composable() override { return to_c(a_.qvfr_composable()); }
    int32_t qvfr_compose_d3d11() override { return to_c(a_.qvfr_compose_d3d11()); }
    int32_t loads_cheeky() override { return to_c(a_.loads_cheeky()); }
    int32_t qvfr_eye_columns() override { return to_c(a_.qvfr_eye_columns()); }
    int32_t qvfr_one_view_size() override { return to_c(a_.qvfr_one_view_size()); }
    int32_t qvfr_separate_views() override { return to_c(a_.qvfr_separate_views()); }
    bool qvfr_eye_extent(uint32_t (&w)[2], uint32_t (&h)[2]) override {
        unsigned uw[2] = {w[0], w[1]}, uh[2] = {h[0], h[1]};
        const bool ok = a_.qvfr_eye_extent(uw, uh);
        for (int e = 0; e < 2; ++e) {
            w[e] = uw[e];
            h[e] = uh[e];
        }
        return ok;
    }
    void* qvfr_views_atlas(void* const (&eyes)[2], float (&uv)[4][4]) override {
        ID3D11Texture2D* const e[2] = {static_cast<ID3D11Texture2D*>(eyes[0]), static_cast<ID3D11Texture2D*>(eyes[1])};
        return a_.qvfr_views_atlas(e, uv);
    }
    int32_t attaches_action_sets() override { return to_c(a_.attaches_action_sets()); }
    // 0 would read as "the core's default" (30): an adapter answering 0 itself is passed as 0 seconds can't be, so it
    // gets 1 instead (no first-party adapter answers 0).
    uint32_t hang_watch_s() override {
        const unsigned s = a_.hang_watch_s();
        return s ? s : 1;
    }

    bool qvfr_start(const SqvfrQvfrStart& s) override { return a_.qvfr_start(from_c(s)); }
    void qvfr_set_gaze(const float (&tan)[2][2], const int32_t (&valid)[2], bool gazeSample) override {
        const bool v[2] = {valid[0] != 0, valid[1] != 0};
        a_.qvfr_set_gaze(tan, v, gazeSample);
    }
    void qvfr_set_live_layout(const SqvfrLayout& live) override { a_.qvfr_set_live_layout(from_c(live)); }
    int32_t qvfr_live_resolution() override { return to_c(a_.qvfr_live_resolution()); }
    void qvfr_set_live(double fx, double fy, double cull, bool gaze) override { a_.qvfr_set_live(fx, fy, cull, gaze); }
    void qvfr_set_focus_mask(bool on, const SqvfrFocusArea& area, int32_t margin) override {
        a_.qvfr_set_focus_mask(on, from_c(area), margin);
    }
    void qvfr_set_focus_visible(const float (&visible)[4]) override { a_.qvfr_set_focus_visible(visible); }
    void qvfr_set_focus_corridor(bool on, int32_t w, int32_t h, int32_t spread, int32_t blend, bool show) override {
        a_.qvfr_set_focus_corridor(on, w, h, spread, blend, show);
    }
    void qvfr_set_object_detail(int32_t percent) override { a_.qvfr_set_object_detail(percent); }
    void qvfr_set_dlss(const SqvfrDlssSettings& d) override { a_.qvfr_set_dlss(from_c(d)); }
    void qvfr_set_nr(const SqvfrNrSettings& n) override { a_.qvfr_set_nr(from_c(n)); }
    bool qvfr_dlss_held() override { return a_.qvfr_dlss_held(); }
    void qvfr_target_sizes(int32_t (&render)[2], int32_t (&output)[2]) override {
        int r[2] = {}, o[2] = {};
        a_.qvfr_target_sizes(r, o);
        for (int i = 0; i < 2; ++i) {
            render[i] = r[i];
            output[i] = o[i];
        }
    }
    bool qvfr_take_frame(SqvfrViews4Frame& c) override {
        Views4Frame f = from_c(c);
        if (!a_.qvfr_take_frame(f))
            return false;
        c = to_c(f);
        return true;
    }
    bool qvfr_latest_frame(SqvfrViews4Frame& c) override {
        Views4Frame f = from_c(c);
        if (!a_.qvfr_latest_frame(f))
            return false;
        c = to_c(f);
        return true;
    }
    int32_t qvfr_copies_layout(const wchar_t* ini) override { return to_c(a_.qvfr_copies_layout(ini)); }
    double qvfr_fixation_m() override { return a_.qvfr_fixation_m(); }
    int32_t qvfr_wants_game_image() override { return to_c(a_.qvfr_wants_game_image()); }
    void qvfr_game_image(void* image, const int32_t (&rects)[4][4]) override {
        int r[4][4];
        for (int v = 0; v < 4; ++v)
            for (int i = 0; i < 4; ++i)
                r[v][i] = rects[v][i];
        a_.qvfr_game_image(static_cast<ID3D11Texture2D*>(image), r);
    }
    void qvfr_note_views(const SqvfrView* views, uint32_t count) override {
        XrView x[kViewsMax];
        const uint32_t n = views ? (count < kViewsMax ? count : kViewsMax) : 0;
        for (uint32_t i = 0; i < n; ++i)
            x[i] = xr_view_from_c(views[i]);
        a_.qvfr_note_views(n ? x : nullptr, n);
    }
    void qvfr_jitter_views(SqvfrView* views, uint32_t count) override {
        if (!views)
            return;
        XrView x[kViewsMax];
        const uint32_t n = count < kViewsMax ? count : kViewsMax;
        for (uint32_t i = 0; i < n; ++i)
            x[i] = xr_view_from_c(views[i]);
        a_.qvfr_jitter_views(x, n);
        for (uint32_t i = 0; i < n; ++i)
            views[i] = to_c(x[i]);
    }
    void visibility_mask_taken(uint32_t view) override { a_.visibility_mask_taken(view); }
    void qvfr_frame_end() override { a_.qvfr_frame_end(); }
    int32_t qvfr_multi_view() override {
        switch (a_.qvfr_multi_view()) {
            case games::QvfrMultiView::nvidia: return SQVFR_MULTI_VIEW_NVIDIA;
            case games::QvfrMultiView::software: return SQVFR_MULTI_VIEW_SOFTWARE;
            case games::QvfrMultiView::fallback: return SQVFR_MULTI_VIEW_FALLBACK;
            case games::QvfrMultiView::eyesOnly: return SQVFR_MULTI_VIEW_EYES_ONLY;
            default: return SQVFR_MULTI_VIEW_NONE;
        }
    }
    bool qvfr_car_light_costs(double& gpu, double& cpu) override { return a_.qvfr_car_light_costs(gpu, cpu); }

    void early_start(const wchar_t* ini) override { a_.early_start(ini); }
    void instance_created(const wchar_t* ini, const wchar_t* mode) override { a_.instance_created(ini, mode); }
    void session_created(bool probed) override { a_.session_created(probed); }
    void d3d11_session(void* device) override { a_.d3d11_session(static_cast<ID3D11Device*>(device)); }
    int32_t observes_d3d11() override { return to_c(a_.observes_d3d11()); }
    int32_t wants_gaze() override { return to_c(a_.wants_gaze()); }
    int32_t shows_overlay() override { return to_c(a_.shows_overlay()); }
    void live_view(const SqvfrLiveView& v) override { a_.live_view(from_c(v)); }
    void qvfr_set_look(double feather, double radius, bool border, const float (&color)[4]) override {
        a_.qvfr_set_look(feather, radius, border, color);
    }
    void session_running(bool running) override { a_.session_running(running); }
    void session_destroyed() override { a_.session_destroyed(); }
    void frame_end() override { a_.frame_end(); }
    void swapchain_acquired(uint64_t chain, uint32_t index, void* enumerate) override {
        a_.swapchain_acquired(reinterpret_cast<XrSwapchain>(chain), index,
                              reinterpret_cast<PFN_xrEnumerateSwapchainImages>(enumerate));
    }
    void swapchain_releasing(uint64_t chain) override { a_.swapchain_releasing(reinterpret_cast<XrSwapchain>(chain)); }
    void swapchain_destroyed(uint64_t chain) override { a_.swapchain_destroyed(reinterpret_cast<XrSwapchain>(chain)); }
    const void* adjust_frame_end(const void* info, void* enumerate) override {
        return a_.adjust_frame_end(static_cast<const XrFrameEndInfo*>(info),
                                   reinterpret_cast<PFN_xrEnumerateSwapchainImages>(enumerate));
    }

private:
    static constexpr uint32_t kViewsMax = 16;
    games::GameAdapter& a_;
};

}  // namespace simqvfr::mod_api
