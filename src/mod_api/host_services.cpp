#include "host_services.h"

#include <windows.h>

#include <MinHook.h>

#include <d3d11.h>

#include <cstring>
#include <filesystem>
#include <new>
#include <string>

#include "../dev_build.h"
#include "../engine/code_patch.h"
#include "../engine/context_hooks.h"
#include "../engine/nvapi_stereo.h"
#include "../games/lmu/mvr_probe.h"
#include "../log.h"
#include "../module_path.h"
#include "../probe/aftermath.h"
#include "../probe/hang_watch.h"
#include "../probe/pass_census.h"
#include "../probe/probe.h"
#include "../probe/run_exit.h"
#include "../quad/car_lighting.h"
#include "../quad/eye_off.h"
#include "../quad/showcase.h"

namespace simqvfr::mod_api {

namespace {

int32_t b(bool v) { return v ? 1 : 0; }

void host_log(int32_t level, const char* message) {
    if (!message)
        return;
    if (level == SQVFR_LOG_DEBUG) {
        if (log_debug())
            log_line(message);
        return;
    }
    log_line(message);
}

// The launch's ini: the launcher's run copy (SIM_QVFR_INI), read once.
const wchar_t* host_ini_path() {
    static const std::wstring ini = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_INI", path, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
    }();
    return ini.c_str();
}

const wchar_t* host_captures_dir() {
    static const std::wstring dir = captures_directory();
    return dir.c_str();
}

int32_t host_hook(void* target, void* detour, void** original) { return b(hook_function(target, detour, original)); }

int32_t host_unhook(void* target) { return b(unhook_function(target)); }

uintptr_t host_module_base() { return module_base(); }

int32_t host_code_matches(uintptr_t rva, const uint8_t* anchor, size_t size) {
    return b(anchor && code_matches(rva, anchor, size));
}

int32_t host_code_write(uintptr_t rva, const void* bytes, size_t size) { return b(bytes && code_write(rva, bytes, size)); }

int32_t host_code_redirect(uintptr_t rva, const uint8_t* anchor, size_t size, const uint8_t* stub, size_t stubSize) {
    return b(anchor && stub && code_redirect(rva, anchor, size, stub, stubSize));
}

uintptr_t host_vtable_slot(uintptr_t slotRva) { return vtable_slot(slotRva); }

int32_t host_vtable_slot_replace(uintptr_t slotRva, uintptr_t expectedRva, const void* replacement) {
    return b(replacement && vtable_slot_replace(slotRva, expectedRva, replacement));
}

int32_t host_readable(const void* address, size_t size) { return b(address && readable(address, size)); }

int32_t host_request_game_exit(const char* why) { return request_game_exit(why ? why : "a mod asked"); }

int32_t host_log_debug_on() { return b(log_debug()); }

const wchar_t* host_log_dir() {
    static const std::wstring dir = log_directory();
    return dir.c_str();
}

// ---- The D3D11 context hooks: the mod's C observers behind the core's (one install at a time, context_hooks.h) ----

namespace ch = context_hooks;

static_assert(int{SQVFR_D3D11_UNMAP} == int{ch::kUnmap} && int{SQVFR_D3D11_DRAW_INSTANCED} == int{ch::kDrawInstanced} &&
                  int{SQVFR_D3D11_PS_SET_CONSTANT_BUFFERS1} == int{ch::kPSSetConstantBuffers1},
              "mod_api.h's D3D11 entries follow context_hooks.h's");
static_assert(sizeof(SqvfrDrawCall) == sizeof(ch::DrawCall), "SqvfrDrawCall mirrors context_hooks::DrawCall");

SqvfrD3D11Observers modObservers = {};

void t_targets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    modObservers.targets(c, n, reinterpret_cast<void* const*>(rtvs), dsv);
}
void t_draw(ID3D11DeviceContext* c, ch::Entry e, UINT count, UINT instances) { modObservers.draw(c, e, count, instances); }
void t_execute(ID3D11DeviceContext* c, ID3D11CommandList* list) { modObservers.execute(c, list); }
bool t_draw_replace(ID3D11DeviceContext* c, ch::Entry e, const ch::DrawCall& call) {
    const SqvfrDrawCall d = {call.count, call.instances, call.start, call.startInstance, call.baseVertex};
    return modObservers.draw_replace(c, e, &d) != 0;
}
void t_draw_after(ID3D11DeviceContext* c, ch::Entry e, UINT count, UINT instances, UINT start) {
    modObservers.draw_after(c, e, count, instances, start);
}
bool t_viewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* in, D3D11_VIEWPORT* out) {
    return modObservers.viewports(c, n, in, out) != 0;
}
bool t_scissors(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* in, D3D11_RECT* out) {
    return modObservers.scissors(c, n, in, out) != 0;
}
bool t_constant_buffers(ID3D11DeviceContext* c, bool pixel, UINT start, UINT n, ID3D11Buffer* const* in,
                        ID3D11Buffer** out) {
    return modObservers.constant_buffers(c, b(pixel), start, n, reinterpret_cast<void* const*>(in),
                                         reinterpret_cast<void**>(out)) != 0;
}
void t_unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) { modObservers.unmap(c, r, sub); }

int32_t host_d3d11_hooks_install(void* context, uint32_t wanted, const SqvfrD3D11Observers* observers) {
    if (!context || !observers || ch::installed())
        return 0;
    modObservers = *observers;
    ch::Observers o;
    o.targets = modObservers.targets ? t_targets : nullptr;
    o.draw = modObservers.draw ? t_draw : nullptr;
    o.execute = modObservers.execute ? t_execute : nullptr;
    o.draw_replace = modObservers.draw_replace ? t_draw_replace : nullptr;
    o.draw_after = modObservers.draw_after ? t_draw_after : nullptr;
    o.viewports = modObservers.viewports ? t_viewports : nullptr;
    o.scissors = modObservers.scissors ? t_scissors : nullptr;
    o.constant_buffers = modObservers.constant_buffers ? t_constant_buffers : nullptr;
    o.unmap = modObservers.unmap ? t_unmap : nullptr;
    return b(ch::install(static_cast<ID3D11DeviceContext*>(context), wanted, o));
}
void host_d3d11_hooks_rescan() { ch::rescan(); }
void host_d3d11_hooks_uninstall() { ch::uninstall(); }
int32_t host_d3d11_hooks_installed() { return b(ch::installed()); }
void host_d3d11_hooks_bypass(int32_t enter) {
    // A Bypass is a depth count on this thread (context_hooks.cpp): entering constructs one, leaving destroys one.
    alignas(ch::Bypass) static thread_local unsigned char slot[sizeof(ch::Bypass)];
    if (enter)
        new (slot) ch::Bypass;
    else
        std::launder(reinterpret_cast<ch::Bypass*>(slot))->~Bypass();
}

// ---- The multi-view shader service, NVAPI, Aftermath, the hang watch, the shared switches ----

int32_t host_mvs_install(const wchar_t* ini, int32_t attachTwins, int32_t nameShaders) {
    return b(lmu::mvr_probe_install(ini, attachTwins != 0, nameShaders != 0));
}
void host_mvs_device(void* device) { lmu::mvr_probe_device(static_cast<ID3D11Device*>(device)); }
void host_mvs_option(int32_t option, int32_t on) {
    switch (option) {
        case SQVFR_MVS_POSITION_TWINS: lmu::mvr_probe_position_twins(on != 0); break;
        case SQVFR_MVS_OWN_PIXEL_LOADS: lmu::mvr_probe_own_pixel_loads(on != 0); break;
        case SQVFR_MVS_TINT_ROWS: lmu::mvr_probe_tint_rows(on != 0); break;
        case SQVFR_MVS_CB_ROWS: lmu::mvr_probe_cb_rows(on != 0); break;
        default: break;
    }
}
void host_mvs_frame_end() { lmu::mvr_probe_frame_end(); }
int32_t host_mvs_backend() {
    switch (lmu::mvr_probe_backend()) {
        case lmu::MvrBackend::nvapi: return SQVFR_MVS_BACKEND_NVAPI;
        case lmu::MvrBackend::instanced: return SQVFR_MVS_BACKEND_INSTANCED;
        default: return SQVFR_MVS_BACKEND_NONE;
    }
}

void host_nvapi_query(void* device, SqvfrNvapiSupport* out) {
    if (!out)
        return;
    const nvapi::Support s = nvapi::query(static_cast<ID3D11Device*>(device));
    *out = {b(s.loaded), b(s.singlePassStereo), b(s.xyzw), b(s.multiView), s.status};
}
int32_t host_nvapi_set_two_views(void* context, int32_t on, int32_t independentMasks, uint32_t targetOffset) {
    return nvapi::set_two_views(static_cast<ID3D11DeviceContext*>(context), on != 0, independentMasks != 0, targetOffset);
}
const char* host_nvapi_status_name(int32_t status) { return nvapi::status_name(status); }

int32_t host_aftermath_built() { return b(aftermath::built()); }
int32_t host_aftermath_enable_dumps() { return b(aftermath::enable_dumps()); }
int32_t host_aftermath_attach(void* device, void* context, int32_t shaderErrors) {
    return b(aftermath::attach(static_cast<ID3D11Device*>(device), static_cast<ID3D11DeviceContext*>(context),
                               shaderErrors != 0));
}
void host_aftermath_mark(uint64_t code) { aftermath::mark(code); }
int32_t host_aftermath_marking() { return b(aftermath::marking()); }
void host_aftermath_set_marker_names(void (*name)(uint64_t, char*, unsigned)) { aftermath::set_marker_names(name); }
void host_aftermath_after_removal(unsigned waitMs) { aftermath::after_removal(waitMs); }
void host_aftermath_detach() { aftermath::detach(); }

void host_hang_watch_on_fire(void (*fn)(void)) { hang_watch_on_fire(fn); }

int32_t host_stock_wanted() { return b(stock_wanted()); }
void host_set_stock_active(int32_t on) { set_stock_active(on != 0); }
void host_note_scene_frame() { note_scene_frame(); }
int32_t host_eye_off() { return eye_off(); }
uint64_t host_car_lighting_bits() { return car_lighting_current_bits(); }

// A mod or setting name: a-z, 0-9, _ (and - in a mod id), up to 32 (launcher/mod_catalog.h valid_mod_name).
bool setting_name(const char* s, bool dash) {
    if (!s || !*s)
        return false;
    size_t n = 0;
    for (; s[n]; ++n)
        if (n >= 32 || !((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '_' || (dash && s[n] == '-')))
            return false;
    return true;
}

int32_t host_mod_setting(const SqvfrHost* host, const char* key, char* out, uint32_t size) {
    if (!host || !out || size == 0)
        return -1;
    out[0] = '\0';
    if (!setting_name(host->modId, true) || !setting_name(key, false))
        return -1;
    const std::wstring section = L"mod_" + std::wstring(host->modId, host->modId + strlen(host->modId));
    const std::wstring name(key, key + strlen(key));
    wchar_t value[128] = {};
    const DWORD n = GetPrivateProfileStringW(section.c_str(), name.c_str(), L"", value, 128, mods_ini().c_str());
    if (n == 0 || n + 1 > size)
        return -1;
    for (DWORD i = 0; i < n; ++i)
        out[i] = value[i] < 0x80 ? static_cast<char>(value[i]) : '?';
    out[n] = '\0';
    return static_cast<int32_t>(n);
}


int32_t host_hook_create(void* target, void* detour, void** original) { return hook_create(target, detour, original); }
int32_t host_hook_enable(void* target) { return hook_enable(target); }
int32_t host_hook_disable(void* target) { return hook_disable(target); }
int32_t host_hook_remove(void* target) { return hook_remove(target); }
int32_t host_hook_queue_enable(void* target) { return hook_queue_enable(target); }
int32_t host_hook_apply_queued() { return hook_apply_queued(); }
const char* host_hook_status_name(int32_t status) { return hook_status_name(status); }

int32_t host_pass_census_installed() { return b(pass_census_installed()); }
void host_pass_census_barrier() { pass_census_barrier_after_next_dispatch(); }
void host_pass_census_set_view_columns(const int32_t left[4], const int32_t width[4]) {
    if (!left || !width)
        return;
    const int l[4] = {left[0], left[1], left[2], left[3]}, w[4] = {width[0], width[1], width[2], width[3]};
    pass_census_set_view_columns(l, w);
}
int32_t host_probe_set_observer(const char* name, void (*fn)(void* self)) { return b(name && probe_set_observer(name, fn)); }

}  // namespace

std::wstring mods_ini() {
    wchar_t env[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_INI", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH && std::filesystem::path(env).is_absolute())
        return env;
    return (std::filesystem::path(module_folder(reinterpret_cast<const void*>(&mods_ini))) / L"sim-qvfr.ini").wstring();
}

const SqvfrHost& host_table() {
    static const SqvfrHost host = [] {
        SqvfrHost h = {};
        h.size = sizeof(SqvfrHost);
        h.abi = SQVFR_ABI_VERSION;
        h.flags = kDevBuild ? SQVFR_HOST_DEV_BUILD : 0u;
        h.gameId = "";  // PluginAdapter::load names the game in its own copy
        h.log = host_log;
        h.ini_path = host_ini_path;
        h.captures_dir = host_captures_dir;
        h.hook = host_hook;
        h.unhook = host_unhook;
        h.module_base = host_module_base;
        h.code_matches = host_code_matches;
        h.code_write = host_code_write;
        h.code_redirect = host_code_redirect;
        h.vtable_slot = host_vtable_slot;
        h.vtable_slot_replace = host_vtable_slot_replace;
        h.readable = host_readable;
        h.request_game_exit = host_request_game_exit;
        h.log_debug_on = host_log_debug_on;
        h.log_dir = host_log_dir;
        h.d3d11_hooks_install = host_d3d11_hooks_install;
        h.d3d11_hooks_rescan = host_d3d11_hooks_rescan;
        h.d3d11_hooks_uninstall = host_d3d11_hooks_uninstall;
        h.d3d11_hooks_installed = host_d3d11_hooks_installed;
        h.d3d11_hooks_bypass = host_d3d11_hooks_bypass;
        h.multi_view_shaders_install = host_mvs_install;
        h.multi_view_shaders_device = host_mvs_device;
        h.multi_view_shaders_option = host_mvs_option;
        h.multi_view_shaders_frame_end = host_mvs_frame_end;
        h.multi_view_shaders_backend = host_mvs_backend;
        h.nvapi_query = host_nvapi_query;
        h.nvapi_set_two_views = host_nvapi_set_two_views;
        h.nvapi_status_name = host_nvapi_status_name;
        h.aftermath_built = host_aftermath_built;
        h.aftermath_enable_dumps = host_aftermath_enable_dumps;
        h.aftermath_attach = host_aftermath_attach;
        h.aftermath_mark = host_aftermath_mark;
        h.aftermath_marking = host_aftermath_marking;
        h.aftermath_set_marker_names = host_aftermath_set_marker_names;
        h.aftermath_after_removal = host_aftermath_after_removal;
        h.aftermath_detach = host_aftermath_detach;
        h.hang_watch_on_fire = host_hang_watch_on_fire;
        h.stock_wanted = host_stock_wanted;
        h.set_stock_active = host_set_stock_active;
        h.note_scene_frame = host_note_scene_frame;
        h.eye_off = host_eye_off;
        h.car_lighting_bits = host_car_lighting_bits;
        h.modId = "";  // PluginAdapter::load names the mod in its own copy
        h.mod_setting = host_mod_setting;
        h.hook_create = host_hook_create;
        h.hook_enable = host_hook_enable;
        h.hook_disable = host_hook_disable;
        h.hook_remove = host_hook_remove;
        h.hook_queue_enable = host_hook_queue_enable;
        h.hook_apply_queued = host_hook_apply_queued;
        h.hook_status_name = host_hook_status_name;
        h.pass_census_installed = host_pass_census_installed;
        h.pass_census_barrier_after_next_dispatch = host_pass_census_barrier;
        h.pass_census_set_view_columns = host_pass_census_set_view_columns;
        h.probe_set_observer = host_probe_set_observer;
        return h;
    }();
    return host;
}

}  // namespace simqvfr::mod_api
