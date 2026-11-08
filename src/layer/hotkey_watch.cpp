// Hotkeys: the wheel and controller thread, keys held, and the hints and footer that name them.
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "input/hotkeys.h"
#include "input/pad_input.h"
#include "log.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

// ---- Hotkeys ---------------------------------------------------------------------------------
// [hotkeys] (input/hotkeys.h), reloaded live: each action's keyboard combination, held while ACE has focus,
// and its wheel or controller button, read by a thread of its own through DirectInput (input/pad_input.h)
// whenever one is bound. The thread polls every kPadPollMs and publishes the actions held as bits.
constexpr int kPadPollMs = 8, kPadRescanMs = 3000;

bool pad_bound(const Hotkeys& h) {
    for (const Hotkey& k : h)
        if (k.pad.set())
            return true;
    return false;
}

void pad_watch_run() {
    // DirectInput wants a top-level window of this process; a hidden one of the thread's own.
    HWND window = CreateWindowExW(0, L"STATIC", L"Sim-QVFR input", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    PadInput pads;
    if (!window || !pads.open(window)) {
        log_line("hotkeys: DirectInput not available; wheel and controller buttons do nothing");
        if (window)
            DestroyWindow(window);
        return;
    }
    {
        std::wstring found;
        for (const auto& [id, name] : pads.devices())
            found += (found.empty() ? L"" : L", ") + name + L" (" + id + L")";
        logf("hotkeys: game controllers: %ls", found.empty() ? L"none" : found.c_str());
    }
    int64_t nextRescan = qpc_now() + qpc_ms(kPadRescanMs);
    std::unique_lock lock(padWatch.mutex);
    while (!padWatch.stop) {
        const Hotkeys bindings = padWatch.bindings;
        lock.unlock();
        const std::vector<PadHeld> held = pads.poll();
        const auto devices = pads.devices();
        uint32_t bits = 0;
        bool missing = false;
        for (int a = 0; a < kHotkeyActions; ++a) {
            const PadControl& want = bindings[a].pad;
            if (!want.set())
                continue;
            bool connected = false;
            for (const PadHeld& h : held)
                if (_wcsicmp(h.device.c_str(), want.device.c_str()) == 0 && h.control == want.control)
                    bits |= 1u << a;
            for (const auto& [id, name] : devices)
                connected |= _wcsicmp(id.c_str(), want.device.c_str()) == 0;
            missing |= !connected;
        }
        padWatch.held.store(bits, std::memory_order_relaxed);
        if (missing && qpc_now() >= nextRescan) {  // a bound device plugged in since
            pads.rescan();
            nextRescan = qpc_now() + qpc_ms(kPadRescanMs);
        }
        lock.lock();
        padWatch.wake.wait_for(lock, std::chrono::milliseconds(kPadPollMs), [] { return padWatch.stop; });
    }
    lock.unlock();
    pads.close();
    DestroyWindow(window);
}

}  // namespace

// Starts the thread once a wheel or controller button is bound (from the frame thread, each frame).
void pad_watch_ensure() {
    if (!pad_bound(hotkeys))
        return;
    std::lock_guard guard(padWatch.mutex);
    if (!padWatch.thread.joinable()) {
        padWatch.stop = false;
        padWatch.bindings = hotkeys;
        padWatch.thread = std::thread(pad_watch_run);
    }
}

void pad_watch_stop() {
    std::thread thread;
    {
        std::lock_guard guard(padWatch.mutex);
        padWatch.stop = true;
        thread.swap(padWatch.thread);
    }
    padWatch.wake.notify_all();
    if (thread.joinable())
        thread.join();
    padWatch.held.store(0);
}

void hotkeys_changed(const Hotkeys& h) {
    if (h == hotkeys)
        return;
    hotkeys = h;
    {
        std::lock_guard guard(padWatch.mutex);
        padWatch.bindings = h;
    }
    std::wstring line;
    for (int a = 0; a < kHotkeyActions; ++a) {
        const std::wstring key = key_text(h[a].key), pad = control_text(h[a].pad.control);
        line += std::wstring(a ? L", " : L"") + hotkey_ini_key(static_cast<HotkeyAction>(a)) + L"=" + (key.empty() ? L"-" : key) +
                (pad.empty() ? L"" : L" / " + pad);
    }
    logf("hotkeys: %ls", line.c_str());
}

// Exactly `key`'s modifiers and its key held, while ACE has focus.
bool key_held(const KeyCombo& key) {
    if (!key.set() || !game_focused())
        return false;
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return down(key.vk) && down(VK_CONTROL) == key.ctrl && down(VK_MENU) == key.alt && down(VK_SHIFT) == key.shift;
}

bool pad_held(HotkeyAction action) {
    return (padWatch.held.load(std::memory_order_relaxed) >> static_cast<int>(action)) & 1;
}

bool hotkey_held(HotkeyAction action) { return key_held(hotkeys[static_cast<int>(action)].key) || pad_held(action); }

// The settings menu's hint: how to select and change, and how to close.
std::wstring menu_hint() {
    const Hotkeys defaults = default_hotkeys();
    bool defaultKeys = true, padKeys = false;
    for (int a = static_cast<int>(HotkeyAction::MenuUp); a <= static_cast<int>(HotkeyAction::MenuRight); ++a) {
        defaultKeys &= hotkeys[a].key == defaults[a].key;
        padKeys |= hotkeys[a].pad.set();
    }
    const Hotkey& toggle = hotkeys[static_cast<int>(HotkeyAction::MenuToggle)];
    std::wstring hint = defaultKeys ? L"Ctrl+Alt+arrows" : L"Your menu hotkeys";
    hint += padKeys ? L", wheel buttons or right stick: select and change" : L", or right stick: select and change";
    hint += toggle.key.set() || toggle.pad.set() ? L" · open/close hotkey or left stick click: close" : L" · left stick click: close";
    return hint;
}

// The menu's hotkeys footer ([menu] hotkey_footer): the in-game actions' keys and wheel buttons, those that apply
// (the visor with the helmet on, the CPU capture with cpu_profile, the frame capture with d3d11_capture_hotkey); empty
// when none is bound.
std::wstring hotkey_footer(bool helmetOn) {
    struct Entry {
        HotkeyAction action;
        const wchar_t* name;
        bool shown;
    };
    const Entry entries[] = {{HotkeyAction::Overlay, L"Overlay", true},
                             {HotkeyAction::Visor, L"Visor", helmetOn},
                             {HotkeyAction::CpuCapture, L"CPU capture", experiment.cpuProfile},
                             {HotkeyAction::FrameCapture, L"Frame capture", experiment.d3d11CaptureHotkey}};
    std::wstring out;
    for (const Entry& e : entries) {
        const Hotkey& h = hotkeys[static_cast<int>(e.action)];
        if (!e.shown || (!h.key.set() && !h.pad.set()))
            continue;
        std::wstring binding = key_text(h.key);
        if (h.pad.set())
            binding += (binding.empty() ? L"" : L" / ") + std::wstring(L"wheel ") + control_text(h.pad.control);
        out += (out.empty() ? L"" : L"  \u00b7  ") + std::wstring(e.name) + L" " + binding;
    }
    return out;
}

}  // namespace simqvfr::layer
