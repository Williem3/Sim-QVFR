// The core's services inside a first-party game mod's own DLL (SIM_QVFR_<GAME>_AS_MOD): the same functions the game's
// code calls when it is built into sim-qvfr.dll, each forwarded to the host table (mod_api.h SqvfrHost) the core
// passed to the mod's entry. So the game's sources stay as they are, and every piece of process-wide state (the log, the
// D3D11 hooks, the multi-view shaders, Aftermath, the hang watch, the layer's switches) stays the core's one copy.
// Compiled into mod DLLs only, never into the core.
#include <windows.h>
#include <d3d11.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "../engine/context_hooks.h"
#include "../engine/nvapi_stereo.h"
#include "../games/identity_adapter.h"
#include "../games/lmu/mvr_probe.h"
#include "../log.h"
#include "../probe/aftermath.h"
#include "../probe/hang_watch.h"
#include "../quad/car_lighting.h"
#include "../quad/eye_off.h"
#include "../quad/showcase.h"
#include "core_shim.h"

namespace {

const SqvfrHost* host = nullptr;

// The host's entry `field`, or null when the core's table doesn't reach it.
template <class Fn> Fn entry(Fn SqvfrHost::* field) {
    if (!host)
        return nullptr;
    const auto offset =
        static_cast<std::size_t>(reinterpret_cast<const char*>(&(host->*field)) - reinterpret_cast<const char*>(host));
    return offset + sizeof(Fn) <= host->size ? host->*field : nullptr;
}

#define SQVFR_CALL(field, fallback, ...) \
    (entry(&SqvfrHost::field) ? entry(&SqvfrHost::field)(__VA_ARGS__) : (fallback))
#define SQVFR_DO(field, ...)                       \
    do {                                           \
        if (auto fn_ = entry(&SqvfrHost::field))   \
            fn_(__VA_ARGS__);                      \
    } while (0)

}  // namespace

namespace sqvfr_shim {

bool bind(const SqvfrHost* h) {
    if (!h || h->abi != SQVFR_ABI_VERSION || h->size < sizeof(SqvfrHost))
        return false;  // an older core: some service this mod calls would be missing
    host = h;
    return true;
}

}  // namespace sqvfr_shim

// ---- log.h ----

namespace simqvfr {

void log_line(const char* message) { SQVFR_DO(log, SQVFR_LOG_INFO, message); }

void logf(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    log_line(text);
}

bool log_debug() { return SQVFR_CALL(log_debug_on, 0) != 0; }

void logd(const char* format, ...) {
    if (!log_debug())
        return;
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    SQVFR_DO(log, SQVFR_LOG_DEBUG, text);
}

std::wstring log_directory() {
    const wchar_t* dir = SQVFR_CALL(log_dir, static_cast<const wchar_t*>(nullptr));
    return dir ? dir : L"";
}

std::wstring captures_directory() {
    const wchar_t* dir = SQVFR_CALL(captures_dir, static_cast<const wchar_t*>(nullptr));
    return dir ? dir : L"";
}

// ---- engine/context_hooks.h: the mod's observers behind the host's C ones ----

namespace context_hooks {

namespace {

Observers modObservers;

void c_targets(void* c, uint32_t n, void* const* rtvs, void* dsv) {
    modObservers.targets(static_cast<ID3D11DeviceContext*>(c), n, reinterpret_cast<ID3D11RenderTargetView* const*>(rtvs),
                         static_cast<ID3D11DepthStencilView*>(dsv));
}
void c_draw(void* c, int32_t e, uint32_t count, uint32_t instances) {
    modObservers.draw(static_cast<ID3D11DeviceContext*>(c), static_cast<Entry>(e), count, instances);
}
void c_execute(void* c, void* list) {
    modObservers.execute(static_cast<ID3D11DeviceContext*>(c), static_cast<ID3D11CommandList*>(list));
}
int32_t c_draw_replace(void* c, int32_t e, const SqvfrDrawCall* call) {
    DrawCall d;
    d.count = call->count;
    d.instances = call->instances;
    d.start = call->start;
    d.startInstance = call->startInstance;
    d.baseVertex = call->baseVertex;
    return modObservers.draw_replace(static_cast<ID3D11DeviceContext*>(c), static_cast<Entry>(e), d) ? 1 : 0;
}
void c_draw_after(void* c, int32_t e, uint32_t count, uint32_t instances, uint32_t start) {
    modObservers.draw_after(static_cast<ID3D11DeviceContext*>(c), static_cast<Entry>(e), count, instances, start);
}
int32_t c_viewports(void* c, uint32_t n, const void* in, void* out) {
    return modObservers.viewports(static_cast<ID3D11DeviceContext*>(c), n, static_cast<const D3D11_VIEWPORT*>(in),
                                  static_cast<D3D11_VIEWPORT*>(out))
               ? 1
               : 0;
}
int32_t c_scissors(void* c, uint32_t n, const void* in, void* out) {
    return modObservers.scissors(static_cast<ID3D11DeviceContext*>(c), n, static_cast<const D3D11_RECT*>(in),
                                 static_cast<D3D11_RECT*>(out))
               ? 1
               : 0;
}
int32_t c_constant_buffers(void* c, int32_t pixel, uint32_t start, uint32_t n, void* const* in, void** out) {
    return modObservers.constant_buffers(static_cast<ID3D11DeviceContext*>(c), pixel != 0, start, n,
                                         reinterpret_cast<ID3D11Buffer* const*>(in), reinterpret_cast<ID3D11Buffer**>(out))
               ? 1
               : 0;
}
void c_unmap(void* c, void* r, uint32_t sub) {
    modObservers.unmap(static_cast<ID3D11DeviceContext*>(c), static_cast<ID3D11Resource*>(r), sub);
}

}  // namespace

bool install(ID3D11DeviceContext* context, uint32_t wanted, const Observers& observers) {
    auto fn = entry(&SqvfrHost::d3d11_hooks_install);
    if (!fn)
        return false;
    modObservers = observers;
    SqvfrD3D11Observers o = {};
    o.targets = observers.targets ? c_targets : nullptr;
    o.draw = observers.draw ? c_draw : nullptr;
    o.execute = observers.execute ? c_execute : nullptr;
    o.draw_replace = observers.draw_replace ? c_draw_replace : nullptr;
    o.draw_after = observers.draw_after ? c_draw_after : nullptr;
    o.viewports = observers.viewports ? c_viewports : nullptr;
    o.scissors = observers.scissors ? c_scissors : nullptr;
    o.constant_buffers = observers.constant_buffers ? c_constant_buffers : nullptr;
    o.unmap = observers.unmap ? c_unmap : nullptr;
    return fn(context, wanted, &o) != 0;
}
void rescan() { SQVFR_DO(d3d11_hooks_rescan); }
void uninstall() { SQVFR_DO(d3d11_hooks_uninstall); }
bool installed() { return SQVFR_CALL(d3d11_hooks_installed, 0) != 0; }
Stats stats() { return {}; }  // the core's own counts are in its log
Bypass::Bypass() { SQVFR_DO(d3d11_hooks_bypass, 1); }
Bypass::~Bypass() { SQVFR_DO(d3d11_hooks_bypass, 0); }

}  // namespace context_hooks

// ---- engine/nvapi_stereo.h ----

namespace nvapi {

Support query(ID3D11Device* device) {
    SqvfrNvapiSupport s = {};
    SQVFR_DO(nvapi_query, device, &s);
    Support out;
    out.loaded = s.loaded != 0;
    out.singlePassStereo = s.singlePassStereo != 0;
    out.xyzw = s.xyzw != 0;
    out.multiView = s.multiView != 0;
    out.status = s.status;
    return out;
}
int set_two_views(ID3D11DeviceContext* context, bool on, bool independentMasks, unsigned targetOffset) {
    return SQVFR_CALL(nvapi_set_two_views, -1, context, on ? 1 : 0, independentMasks ? 1 : 0, targetOffset);
}
const char* status_name(int status) { return SQVFR_CALL(nvapi_status_name, "unknown", status); }

}  // namespace nvapi

// ---- probe/aftermath.h, probe/hang_watch.h ----

namespace aftermath {

bool built() { return SQVFR_CALL(aftermath_built, 0) != 0; }
bool enable_dumps() { return SQVFR_CALL(aftermath_enable_dumps, 0) != 0; }
bool attach(ID3D11Device* device, ID3D11DeviceContext* context, bool shaderErrors) {
    return SQVFR_CALL(aftermath_attach, 0, device, context, shaderErrors ? 1 : 0) != 0;
}
void mark(uint64_t code) { SQVFR_DO(aftermath_mark, code); }
bool marking() { return SQVFR_CALL(aftermath_marking, 0) != 0; }
void set_marker_names(void (*name)(uint64_t code, char* out, unsigned size)) { SQVFR_DO(aftermath_set_marker_names, name); }
void after_removal(unsigned waitMs) { SQVFR_DO(aftermath_after_removal, waitMs); }
void detach() { SQVFR_DO(aftermath_detach); }

}  // namespace aftermath

void hang_watch_on_fire(void (*fn)()) { SQVFR_DO(hang_watch_on_fire, fn); }

// ---- The layer's switches (quad/showcase.h, eye_off.h, car_lighting.h): the core's copy, read through the host ----

bool stock_wanted() { return SQVFR_CALL(stock_wanted, 0) != 0; }
void set_stock_active(bool on) { SQVFR_DO(set_stock_active, on ? 1 : 0); }
void note_scene_frame() { SQVFR_DO(note_scene_frame); }
int eye_off() { return SQVFR_CALL(eye_off, kEyeOffNone); }
uint64_t car_lighting_current_bits() { return SQVFR_CALL(car_lighting_bits, car_lighting_bits(CarLighting{})); }

// ---- games/identity_adapter.h: its one out-of-line method (adapters.cpp in the core; no local analysis in a mod) ----

namespace games {
void IdentityAdapter::instance_created(const wchar_t*, const wchar_t*) {}
}  // namespace games

}  // namespace simqvfr

// ---- games/lmu/mvr_probe.h: the core's multi-view shader service ----

namespace lmu {

bool mvr_probe_install(const wchar_t* ini, bool attachTwins, bool nameShaders) {
    return SQVFR_CALL(multi_view_shaders_install, 0, ini, attachTwins ? 1 : 0, nameShaders ? 1 : 0) != 0;
}
void mvr_probe_device(ID3D11Device* device) { SQVFR_DO(multi_view_shaders_device, device); }
void mvr_probe_position_twins(bool on) { SQVFR_DO(multi_view_shaders_option, SQVFR_MVS_POSITION_TWINS, on ? 1 : 0); }
void mvr_probe_own_pixel_loads(bool on) { SQVFR_DO(multi_view_shaders_option, SQVFR_MVS_OWN_PIXEL_LOADS, on ? 1 : 0); }
void mvr_probe_tint_rows(bool on) { SQVFR_DO(multi_view_shaders_option, SQVFR_MVS_TINT_ROWS, on ? 1 : 0); }
void mvr_probe_cb_rows(bool on) { SQVFR_DO(multi_view_shaders_option, SQVFR_MVS_CB_ROWS, on ? 1 : 0); }
void mvr_probe_frame_end() { SQVFR_DO(multi_view_shaders_frame_end); }
MvrBackend mvr_probe_backend() {
    switch (SQVFR_CALL(multi_view_shaders_backend, SQVFR_MVS_BACKEND_NONE)) {
        case SQVFR_MVS_BACKEND_NVAPI: return MvrBackend::nvapi;
        case SQVFR_MVS_BACKEND_INSTANCED: return MvrBackend::instanced;
        default: return MvrBackend::none;
    }
}

}  // namespace lmu
