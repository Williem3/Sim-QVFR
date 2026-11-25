#include "plugin_adapter.h"

#include <cstddef>
#include <cstring>

#include "convert.h"
#include "host_services.h"

namespace simqvfr::mod_api {

namespace {

// The header every table has, whatever its version: size, abi, self, gameId.
constexpr std::size_t kHeaderSize = offsetof(SqvfrMod, gameId) + sizeof(const char*);

bool answer(int32_t a, bool fallback) { return a < 0 ? fallback : a != 0; }

}  // namespace

std::string table_problem(const SqvfrMod* mod, const games::GameInfo* game) {
    if (!mod)
        return "the mod returned no table";
    if (mod->size < kHeaderSize)
        return "the mod's table is too short (" + std::to_string(mod->size) + " bytes)";
    if (mod->abi != SQVFR_ABI_VERSION)
        return "the mod is built for ABI " + std::to_string(mod->abi) + ", this Sim-QVFR has ABI " +
               std::to_string(SQVFR_ABI_VERSION);
    if (game && (!mod->gameId || _stricmp(mod->gameId, game->id) != 0))
        return std::string("the mod is for game \"") + (mod->gameId ? mod->gameId : "") + "\", not \"" + game->id +
               "\"";
    return {};
}

std::unique_ptr<PluginAdapter> PluginAdapter::create(const games::GameInfo* game, const SqvfrMod* mod,
                                                     std::string& why) {
    why = table_problem(mod, game);
    if (!why.empty())
        return nullptr;
    return std::unique_ptr<PluginAdapter>(new PluginAdapter(game, mod));
}

std::unique_ptr<PluginAdapter> PluginAdapter::load(const games::GameInfo* game, SqvfrModEntry entry,
                                                   std::string& why, const std::string& modId) {
    if (!entry) {
        why = "the mod has no entry point";
        return nullptr;
    }
    auto host = std::make_unique<SqvfrHost>(host_table());
    host->gameId = game ? game->id : "";
    // Its own copy of the id, for the process (the table is, a refused mod's too).
    host->modId = modId.empty() ? "" : (new std::string(modId))->c_str();
    auto adapter = create(game, entry(host.get()), why);
    if (!adapter) {
        (void)host.release();  // a refused mod may still hold it: kept for the process
        return nullptr;
    }
    adapter->host_ = std::move(host);
    return adapter;
}

template <class Fn> Fn PluginAdapter::entry(Fn SqvfrMod::* field) const {
    // The entry's offset in the table, from the member pointer (tables are standard layout).
    const auto offset =
        static_cast<std::size_t>(reinterpret_cast<const char*>(&(mod_->*field)) - reinterpret_cast<const char*>(mod_));
    if (offset + sizeof(Fn) > mod_->size)
        return nullptr;
    return mod_->*field;
}

// ---- Queries: below 0 (or no entry) is GameAdapter's own answer ----

#define SQVFR_QUERY(method, field)                                                       \
    bool PluginAdapter::method() const {                                                 \
        auto fn = entry(&SqvfrMod::field);                                               \
        return fn ? answer(fn(mod_->self), GameAdapter::method()) : GameAdapter::method(); \
    }

SQVFR_QUERY(has_qvfr, has_qvfr)
SQVFR_QUERY(qvfr_composable, qvfr_composable)
SQVFR_QUERY(qvfr_compose_d3d11, qvfr_compose_d3d11)
SQVFR_QUERY(loads_cheeky, loads_cheeky)
SQVFR_QUERY(qvfr_eye_columns, qvfr_eye_columns)
SQVFR_QUERY(qvfr_one_view_size, qvfr_one_view_size)
SQVFR_QUERY(qvfr_separate_views, qvfr_separate_views)
SQVFR_QUERY(attaches_action_sets, attaches_action_sets)
SQVFR_QUERY(qvfr_live_resolution, qvfr_live_resolution)
SQVFR_QUERY(qvfr_wants_game_image, qvfr_wants_game_image)
SQVFR_QUERY(observes_d3d11, observes_d3d11)
SQVFR_QUERY(wants_gaze, wants_gaze)
SQVFR_QUERY(shows_overlay, shows_overlay)

#undef SQVFR_QUERY

bool PluginAdapter::own_qvfr(const wchar_t* ini) const {
    auto fn = entry(&SqvfrMod::own_qvfr);
    return fn ? answer(fn(mod_->self, ini), GameAdapter::own_qvfr(ini)) : GameAdapter::own_qvfr(ini);
}

bool PluginAdapter::qvfr_copies_layout(const wchar_t* ini) const {
    auto fn = entry(&SqvfrMod::qvfr_copies_layout);
    return fn ? answer(fn(mod_->self, ini), GameAdapter::qvfr_copies_layout(ini)) : GameAdapter::qvfr_copies_layout(ini);
}

unsigned PluginAdapter::hang_watch_s() const {
    auto fn = entry(&SqvfrMod::hang_watch_s);
    const unsigned s = fn ? fn(mod_->self) : 0;
    return s ? s : GameAdapter::hang_watch_s();
}

// ---- Everything else ----

void PluginAdapter::views_offered(uint32_t width, uint32_t height) {
    if (auto fn = entry(&SqvfrMod::views_offered))
        fn(mod_->self, width, height);
}

bool PluginAdapter::qvfr_eye_extent(unsigned (&width)[2], unsigned (&height)[2]) {
    auto fn = entry(&SqvfrMod::qvfr_eye_extent);
    if (!fn)
        return false;
    uint32_t w[2] = {width[0], width[1]}, h[2] = {height[0], height[1]};
    const bool ok = fn(mod_->self, w, h) != 0;
    for (int e = 0; e < 2; ++e) {
        width[e] = w[e];
        height[e] = h[e];
    }
    return ok;
}

ID3D11Texture2D* PluginAdapter::qvfr_views_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]) {
    auto fn = entry(&SqvfrMod::qvfr_views_atlas);
    if (!fn)
        return nullptr;
    void* const e[2] = {eyes[0], eyes[1]};
    return static_cast<ID3D11Texture2D*>(fn(mod_->self, e, uv));
}

bool PluginAdapter::qvfr_start(const games::QvfrStart& start) {
    auto fn = entry(&SqvfrMod::qvfr_start);
    if (!fn)
        return false;  // no QVFR: the game runs as it is
    const SqvfrQvfrStart c = to_c(start);
    return fn(mod_->self, &c) != 0;
}

void PluginAdapter::qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool gazeSample) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_gaze)) {
        const int32_t v[2] = {to_c(valid[0]), to_c(valid[1])};
        fn(mod_->self, tan, v, to_c(gazeSample));
    }
}

void PluginAdapter::qvfr_set_live_layout(const Views4Layout& live) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_live_layout)) {
        const SqvfrLayout c = to_c(live);
        fn(mod_->self, &c);
    }
}

void PluginAdapter::qvfr_set_live(double focusFraction, double focusFractionY, double cullScale, bool gaze) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_live))
        fn(mod_->self, focusFraction, focusFractionY, cullScale, to_c(gaze));
}

void PluginAdapter::qvfr_set_focus_mask(bool on, const FocusArea& area, int marginTexels) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_focus_mask)) {
        const SqvfrFocusArea c = to_c(area);
        fn(mod_->self, to_c(on), &c, marginTexels);
    }
}

void PluginAdapter::qvfr_set_focus_visible(const float (&visible)[4]) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_focus_visible))
        fn(mod_->self, visible);
}

void PluginAdapter::qvfr_set_focus_corridor(bool on, int widthM, int heightDm, int spreadDeg, int blendDm, bool show) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_focus_corridor))
        fn(mod_->self, to_c(on), widthM, heightDm, spreadDeg, blendDm, to_c(show));
}

void PluginAdapter::qvfr_set_object_detail(int percent) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_object_detail))
        fn(mod_->self, percent);
}

void PluginAdapter::qvfr_set_dlss(const DlssSettings& settings) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_dlss)) {
        const SqvfrDlssSettings c = to_c(settings);
        fn(mod_->self, &c);
    }
}

void PluginAdapter::qvfr_set_nr(const NrSettings& settings) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_nr)) {
        const SqvfrNrSettings c = to_c(settings);
        fn(mod_->self, &c);
    }
}

bool PluginAdapter::qvfr_dlss_held() {
    auto fn = entry(&SqvfrMod::qvfr_dlss_held);
    return fn && fn(mod_->self) != 0;
}

void PluginAdapter::qvfr_target_sizes(int (&render)[2], int (&output)[2]) {
    int32_t r[2] = {}, o[2] = {};
    if (auto fn = entry(&SqvfrMod::qvfr_target_sizes))
        fn(mod_->self, r, o);
    for (int i = 0; i < 2; ++i) {
        render[i] = r[i];
        output[i] = o[i];
    }
}

bool PluginAdapter::qvfr_take_frame(Views4Frame& frame) {
    auto fn = entry(&SqvfrMod::qvfr_take_frame);
    SqvfrViews4Frame c = to_c(frame);
    if (!fn || fn(mod_->self, &c) == 0)
        return false;
    frame = from_c(c);
    return true;
}

bool PluginAdapter::qvfr_latest_frame(Views4Frame& frame) {
    auto fn = entry(&SqvfrMod::qvfr_latest_frame);
    SqvfrViews4Frame c = to_c(frame);
    if (!fn || fn(mod_->self, &c) == 0)
        return false;
    frame = from_c(c);
    return true;
}

double PluginAdapter::qvfr_fixation_m() const {
    auto fn = entry(&SqvfrMod::qvfr_fixation_m);
    return fn ? fn(mod_->self) : GameAdapter::qvfr_fixation_m();
}

void PluginAdapter::qvfr_game_image(ID3D11Texture2D* image, const int (&rects)[4][4]) {
    if (auto fn = entry(&SqvfrMod::qvfr_game_image)) {
        int32_t r[4][4];
        for (int v = 0; v < 4; ++v)
            for (int i = 0; i < 4; ++i)
                r[v][i] = rects[v][i];
        fn(mod_->self, image, r);
    }
}

namespace {
constexpr uint32_t kViewsMax = 16;  // OpenXR's view configurations have 1, 2 or 4 views; anything more is cut
}

void PluginAdapter::qvfr_note_views(const XrView* views, uint32_t count) {
    auto fn = entry(&SqvfrMod::qvfr_note_views);
    if (!fn)
        return;
    SqvfrView c[kViewsMax];
    const uint32_t n = views ? (count < kViewsMax ? count : kViewsMax) : 0;
    for (uint32_t i = 0; i < n; ++i)
        c[i] = to_c(views[i]);
    fn(mod_->self, n ? c : nullptr, n);
}

void PluginAdapter::qvfr_jitter_views(XrView* views, uint32_t count) {
    auto fn = entry(&SqvfrMod::qvfr_jitter_views);
    if (!fn || !views)
        return;
    SqvfrView c[kViewsMax];
    const uint32_t n = count < kViewsMax ? count : kViewsMax;
    for (uint32_t i = 0; i < n; ++i)
        c[i] = to_c(views[i]);
    fn(mod_->self, c, n);
    for (uint32_t i = 0; i < n; ++i)
        from_c(c[i], views[i]);
}

void PluginAdapter::visibility_mask_taken(uint32_t viewIndex) {
    if (auto fn = entry(&SqvfrMod::visibility_mask_taken))
        fn(mod_->self, viewIndex);
}

void PluginAdapter::qvfr_frame_end() {
    if (auto fn = entry(&SqvfrMod::qvfr_frame_end))
        fn(mod_->self);
}

games::QvfrMultiView PluginAdapter::qvfr_multi_view() const {
    auto fn = entry(&SqvfrMod::qvfr_multi_view);
    if (!fn)
        return GameAdapter::qvfr_multi_view();
    switch (fn(mod_->self)) {
        case SQVFR_MULTI_VIEW_NVIDIA: return games::QvfrMultiView::nvidia;
        case SQVFR_MULTI_VIEW_SOFTWARE: return games::QvfrMultiView::software;
        case SQVFR_MULTI_VIEW_FALLBACK: return games::QvfrMultiView::fallback;
        case SQVFR_MULTI_VIEW_EYES_ONLY: return games::QvfrMultiView::eyesOnly;
        default: return games::QvfrMultiView::none;
    }
}

bool PluginAdapter::qvfr_car_light_costs(double& gpuMs, double& cpuMs) const {
    auto fn = entry(&SqvfrMod::qvfr_car_light_costs);
    if (!fn)
        return GameAdapter::qvfr_car_light_costs(gpuMs, cpuMs);
    return fn(mod_->self, &gpuMs, &cpuMs) != 0;
}

bool PluginAdapter::qvfr_upscaler(const wchar_t*& badge, unsigned long long& evaluations) const {
    auto fn = entry(&SqvfrMod::qvfr_upscaler);
    if (!fn)
        return GameAdapter::qvfr_upscaler(badge, evaluations);
    const wchar_t* b = nullptr;
    uint64_t n = 0;
    if (fn(mod_->self, &b, &n) == 0 || !b)
        return false;
    badge = b;
    evaluations = n;
    return true;
}

void PluginAdapter::early_start(const wchar_t* ini) {
    if (auto fn = entry(&SqvfrMod::early_start))
        fn(mod_->self, ini);
}

void PluginAdapter::instance_created(const wchar_t* ini, const wchar_t* mode) {
    if (auto fn = entry(&SqvfrMod::instance_created))
        fn(mod_->self, ini, mode);
}

void PluginAdapter::session_created(bool probed) {
    if (auto fn = entry(&SqvfrMod::session_created))
        fn(mod_->self, to_c(probed));
}

void PluginAdapter::d3d11_session(ID3D11Device* device) {
    if (auto fn = entry(&SqvfrMod::d3d11_session))
        fn(mod_->self, device);
}

void PluginAdapter::live_view(const LiveView& view) {
    if (auto fn = entry(&SqvfrMod::live_view)) {
        const SqvfrLiveView c = to_c(view);
        fn(mod_->self, &c);
    }
}

void PluginAdapter::qvfr_set_look(double feather, double cornerRadius, bool border, const float (&color)[4]) {
    if (auto fn = entry(&SqvfrMod::qvfr_set_look))
        fn(mod_->self, feather, cornerRadius, to_c(border), color);
}

void PluginAdapter::session_running(bool running) {
    if (auto fn = entry(&SqvfrMod::session_running))
        fn(mod_->self, to_c(running));
}

void PluginAdapter::session_destroyed() {
    if (auto fn = entry(&SqvfrMod::session_destroyed))
        fn(mod_->self);
}

void PluginAdapter::frame_end() {
    if (auto fn = entry(&SqvfrMod::frame_end))
        fn(mod_->self);
}

void PluginAdapter::swapchain_acquired(XrSwapchain chain, uint32_t index, PFN_xrEnumerateSwapchainImages enumerate) {
    if (auto fn = entry(&SqvfrMod::swapchain_acquired))
        fn(mod_->self, reinterpret_cast<uint64_t>(chain), index, reinterpret_cast<void*>(enumerate));
}

void PluginAdapter::swapchain_releasing(XrSwapchain chain) {
    if (auto fn = entry(&SqvfrMod::swapchain_releasing))
        fn(mod_->self, reinterpret_cast<uint64_t>(chain));
}

void PluginAdapter::swapchain_destroyed(XrSwapchain chain) {
    if (auto fn = entry(&SqvfrMod::swapchain_destroyed))
        fn(mod_->self, reinterpret_cast<uint64_t>(chain));
}

const XrFrameEndInfo* PluginAdapter::adjust_frame_end(const XrFrameEndInfo* info,
                                                      PFN_xrEnumerateSwapchainImages enumerate) {
    auto fn = entry(&SqvfrMod::adjust_frame_end);
    if (!fn)
        return info;
    const void* out = fn(mod_->self, info, reinterpret_cast<void*>(enumerate));
    return out ? static_cast<const XrFrameEndInfo*>(out) : info;
}

namespace {

// A mod's SqvfrFocusImages table as the core's FocusImages: an entry beyond the table's size, or null, does nothing
// (a query answers "none").
class PluginFocusImages final : public FocusImages {
public:
    explicit PluginFocusImages(const SqvfrFocusImages* t) : t_(t) {}

    void early(const FocusImagesStart& s) override {
        const SqvfrFocusImagesStart c = to_c(s);
        if (auto fn = entry(&SqvfrFocusImages::early))
            fn(t_->self, &c);
    }
    bool start(const FocusImagesStart& s) override {
        const SqvfrFocusImagesStart c = to_c(s);
        auto fn = entry(&SqvfrFocusImages::start);
        return fn && fn(t_->self, &c) != 0;
    }
    void frame_end() override {
        if (auto fn = entry(&SqvfrFocusImages::frame_end))
            fn(t_->self);
    }
    void set_gaze(const float (&ndc)[2][2]) override {
        if (auto fn = entry(&SqvfrFocusImages::set_gaze))
            fn(t_->self, ndc);
    }
    void set_size(double k, double kY, double render) override {
        if (auto fn = entry(&SqvfrFocusImages::set_size))
            fn(t_->self, k, kY, render);
    }
    void set_box(double box, double boxY) override {
        if (auto fn = entry(&SqvfrFocusImages::set_box))
            fn(t_->self, box, boxY);
    }
    void set_eye_scale(double scale) override {
        if (auto fn = entry(&SqvfrFocusImages::set_eye_scale))
            fn(t_->self, scale);
    }
    void set_stock(bool on) override {
        if (auto fn = entry(&SqvfrFocusImages::set_stock))
            fn(t_->self, to_c(on));
    }
    void set_eye_texture_match(double share, double mips) override {
        if (auto fn = entry(&SqvfrFocusImages::set_eye_texture_match))
            fn(t_->self, share, mips);
    }
    bool focus_image(FocusImage& out) override {
        auto fn = entry(&SqvfrFocusImages::focus_image);
        SqvfrFocusImage c = {};
        if (!fn || !fn(t_->self, &c))
            return false;
        out = from_c(c);
        return true;
    }
    bool eye_region(float& fx, float& fy) override {
        auto fn = entry(&SqvfrFocusImages::eye_region);
        if (!fn) {
            fx = fy = 1;
            return false;
        }
        return fn(t_->self, &fx, &fy) != 0;
    }
    void eye_allow(bool composed) override {
        if (auto fn = entry(&SqvfrFocusImages::eye_allow))
            fn(t_->self, to_c(composed));
    }
    void gpu_scope(bool compositor) override {
        if (auto fn = entry(&SqvfrFocusImages::gpu_scope))
            fn(t_->self, to_c(compositor));
    }
    bool census(unsigned frames) override {
        auto fn = entry(&SqvfrFocusImages::census);
        return fn && fn(t_->self, frames) != 0;
    }
    void set_live_diag(unsigned bits) override {
        if (auto fn = entry(&SqvfrFocusImages::set_live_diag))
            fn(t_->self, bits);
    }
    void set_dlss_presets(int eye, int focus) override {
        if (auto fn = entry(&SqvfrFocusImages::set_dlss_presets))
            fn(t_->self, eye, focus);
    }
    bool dlss_state(FocusDlssState& out) override {
        auto fn = entry(&SqvfrFocusImages::dlss_state);
        SqvfrFocusDlssState c = {};
        if (!fn || !fn(t_->self, &c))
            return false;
        out = from_c(c);
        return true;
    }

private:
    template <class Fn> Fn entry(Fn SqvfrFocusImages::* field) const {
        const auto offset =
            static_cast<std::size_t>(reinterpret_cast<const char*>(&(t_->*field)) - reinterpret_cast<const char*>(t_));
        return offset + sizeof(Fn) <= t_->size ? t_->*field : nullptr;
    }

    const SqvfrFocusImages* t_;
};

}  // namespace

FocusImages* PluginAdapter::focus_images() {
    if (!imagesAsked_) {
        imagesAsked_ = true;
        auto fn = entry(&SqvfrMod::focus_images);
        const SqvfrFocusImages* t = fn ? fn(mod_->self) : nullptr;
        if (t && t->size >= offsetof(SqvfrFocusImages, self) + sizeof(void*))
            images_ = std::make_unique<PluginFocusImages>(t);
    }
    return images_.get();
}

}  // namespace simqvfr::mod_api
