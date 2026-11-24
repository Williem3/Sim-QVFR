// A game mod's C table (mod_api.h) as the layer's GameAdapter (src/games/adapter.h): every method converts its
// arguments to the ABI's plain structs, calls the mod's entry, and converts the answer back. An entry the mod's table
// doesn't reach (beyond its `size`, an older mod) or leaves null gets GameAdapter's default, as does a query answered
// below 0. The game's identity (GameInfo) is the core's: the table only has to name the same game.
#pragma once

#include <memory>
#include <string>

#include "../games/adapter.h"
#include "mod_api.h"

namespace simqvfr::mod_api {

// Why the core refuses `mod` for `game` (null `game`: any), or empty when it takes it: no table, another ABI version, a
// table too short to hold its own header, or another game's.
std::string table_problem(const SqvfrMod* mod, const games::GameInfo* game);

class PluginAdapter final : public games::GameAdapter {
public:
    // Null, with the reason in `why`, when table_problem refuses the table.
    static std::unique_ptr<PluginAdapter> create(const games::GameInfo* game, const SqvfrMod* mod, std::string& why);
    // Calls `entry` with the core's host table (host_services.h) naming `game`, then create. The adapter keeps that
    // table for the mod (a refused mod's is kept too: it may hold on to it).
    static std::unique_ptr<PluginAdapter> load(const games::GameInfo* game, SqvfrModEntry entry, std::string& why,
                                               const std::string& modId = {});

    const games::GameInfo* info() const override { return game_; }
    bool has_qvfr() const override;
    void views_offered(uint32_t width, uint32_t height) override;
    bool own_qvfr(const wchar_t* ini) const override;
    bool qvfr_composable() const override;
    bool qvfr_compose_d3d11() const override;
    bool loads_cheeky() const override;
    bool qvfr_eye_columns() const override;
    bool qvfr_one_view_size() const override;
    bool qvfr_separate_views() const override;
    bool qvfr_eye_extent(unsigned (&width)[2], unsigned (&height)[2]) override;
    ID3D11Texture2D* qvfr_views_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]) override;
    bool attaches_action_sets() const override;
    unsigned hang_watch_s() const override;

    bool qvfr_start(const games::QvfrStart& start) override;
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool gazeSample) override;
    void qvfr_set_live_layout(const Views4Layout& live) override;
    bool qvfr_live_resolution() const override;
    void qvfr_set_live(double focusFraction, double focusFractionY, double cullScale, bool gaze) override;
    void qvfr_set_focus_mask(bool on, const FocusArea& area, int marginTexels) override;
    void qvfr_set_focus_visible(const float (&visible)[4]) override;
    void qvfr_set_focus_corridor(bool on, int widthM, int heightDm, int spreadDeg, int blendDm, bool show) override;
    void qvfr_set_object_detail(int percent) override;
    void qvfr_set_dlss(const DlssSettings& settings) override;
    void qvfr_set_nr(const NrSettings& settings) override;
    bool qvfr_dlss_held() override;
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override;
    bool qvfr_take_frame(Views4Frame& frame) override;
    bool qvfr_latest_frame(Views4Frame& frame) override;
    bool qvfr_copies_layout(const wchar_t* ini) const override;
    double qvfr_fixation_m() const override;
    bool qvfr_wants_game_image() const override;
    void qvfr_game_image(ID3D11Texture2D* image, const int (&rects)[4][4]) override;
    void qvfr_note_views(const XrView* views, uint32_t count) override;
    void qvfr_jitter_views(XrView* views, uint32_t count) override;
    void visibility_mask_taken(uint32_t viewIndex) override;
    void qvfr_frame_end() override;
    games::QvfrMultiView qvfr_multi_view() const override;
    bool qvfr_car_light_costs(double& gpuMs, double& cpuMs) const override;

    void early_start(const wchar_t* ini) override;
    void instance_created(const wchar_t* ini, const wchar_t* mode) override;
    void session_created(bool probed) override;
    void d3d11_session(ID3D11Device* device) override;
    bool observes_d3d11() const override;
    bool wants_gaze() const override;
    bool shows_overlay() const override;
    void live_view(const LiveView& view) override;
    void qvfr_set_look(double feather, double cornerRadius, bool border, const float (&color)[4]) override;
    void session_running(bool running) override;
    void session_destroyed() override;
    void frame_end() override;
    void swapchain_acquired(XrSwapchain chain, uint32_t index, PFN_xrEnumerateSwapchainImages enumerate) override;
    void swapchain_releasing(XrSwapchain chain) override;
    void swapchain_destroyed(XrSwapchain chain) override;
    const XrFrameEndInfo* adjust_frame_end(const XrFrameEndInfo* info, PFN_xrEnumerateSwapchainImages enumerate) override;

    FocusImages* focus_images() override;

    const SqvfrMod& table() const { return *mod_; }

private:
    PluginAdapter(const games::GameInfo* game, const SqvfrMod* mod) : game_(game), mod_(mod) {}
    // The mod's entry `field`, or null when its table is too short for it (or it left it null).
    template <class Fn> Fn entry(Fn SqvfrMod::* field) const;

    const games::GameInfo* game_;
    const SqvfrMod* mod_;
    std::unique_ptr<SqvfrHost> host_;  // the table `load` passed to the mod
    std::unique_ptr<FocusImages> images_;  // the mod's focus_images table as the core's FocusImages (first asked)
    bool imagesAsked_ = false;
};

}  // namespace simqvfr::mod_api
