// Wheels, button boxes and game controllers through DirectInput 8, read in the background (no focus needed):
// the hotkeys' wheel buttons (hotkeys.h) in the layer, and the launcher's Hotkeys pane catching a press.
// Link dinput8 and dxguid.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace simqvfr {

struct PadHeld {
    std::wstring device, name;  // hotkeys.h's PadControl device ("VVVV_PPPP") and the device's name
    int control = -1;           // hotkeys.h's control numbering
};

class PadInput {
public:
    PadInput();
    ~PadInput();
    PadInput(const PadInput&) = delete;
    PadInput& operator=(const PadInput&) = delete;

    // DirectInput and the attached game controllers, for `window` (a top-level window of this process). False
    // without DirectInput.
    bool open(HWND window);
    // The attached game controllers again (one plugged in since).
    void rescan();
    void close();
    // Every control held right now, on every device.
    std::vector<PadHeld> poll();
    // The devices found: id and name.
    std::vector<std::pair<std::wstring, std::wstring>> devices() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace simqvfr
