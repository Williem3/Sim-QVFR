#include "pad_input.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include "hotkeys.h"

namespace simqvfr {

struct PadInput::Impl {
    IDirectInput8W* input = nullptr;
    HWND window = nullptr;
    struct Device {
        IDirectInputDevice8W* device = nullptr;
        std::wstring id, name;
    };
    std::vector<Device> devices;

    void release_devices() {
        for (Device& d : devices) {
            d.device->Unacquire();
            d.device->Release();
        }
        devices.clear();
    }

    static BOOL CALLBACK found(LPCDIDEVICEINSTANCEW instance, LPVOID context) {
        Impl* self = static_cast<Impl*>(context);
        IDirectInputDevice8W* device = nullptr;
        if (FAILED(self->input->CreateDevice(instance->guidInstance, &device, nullptr)))
            return DIENUM_CONTINUE;
        if (FAILED(device->SetDataFormat(&c_dfDIJoystick2)) ||
            FAILED(device->SetCooperativeLevel(self->window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            device->Release();
            return DIENUM_CONTINUE;
        }
        device->Acquire();
        // A HID device's product GUID starts with its USB product and vendor IDs.
        wchar_t id[16];
        swprintf_s(id, L"%04X_%04X", LOWORD(instance->guidProduct.Data1), HIWORD(instance->guidProduct.Data1));
        self->devices.push_back({device, id, instance->tszProductName});
        return DIENUM_CONTINUE;
    }

    void scan() {
        release_devices();
        if (input)
            input->EnumDevices(DI8DEVCLASS_GAMECTRL, &Impl::found, this, DIEDFL_ATTACHEDONLY);
    }
};

PadInput::PadInput() : impl_(std::make_unique<Impl>()) {}

PadInput::~PadInput() { close(); }

bool PadInput::open(HWND window) {
    close();
    impl_->window = window;
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&impl_->input), nullptr))) {
        impl_->input = nullptr;
        return false;
    }
    impl_->scan();
    return true;
}

void PadInput::rescan() { impl_->scan(); }

void PadInput::close() {
    impl_->release_devices();
    if (impl_->input)
        impl_->input->Release();
    impl_->input = nullptr;
}

std::vector<PadHeld> PadInput::poll() {
    std::vector<PadHeld> held;
    for (Impl::Device& d : impl_->devices) {
        DIJOYSTATE2 state{};
        d.device->Poll();
        HRESULT hr = d.device->GetDeviceState(sizeof(state), &state);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {  // unplugged, or lost to another app's exclusive use
            d.device->Acquire();
            continue;
        }
        if (FAILED(hr))
            continue;
        for (int b = 0; b < kPadButtons; ++b)
            if (state.rgbButtons[b] & 0x80)
                held.push_back({d.id, d.name, b});
        for (int h = 0; h < kPadHats; ++h) {
            const unsigned bits = hat_directions(state.rgdwPOV[h]);
            for (int dir = 0; dir < 4; ++dir)
                if (bits & (1u << dir))
                    held.push_back({d.id, d.name, kPadHatBase + h * 4 + dir});
        }
    }
    return held;
}

std::vector<std::pair<std::wstring, std::wstring>> PadInput::devices() const {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const Impl::Device& d : impl_->devices)
        out.emplace_back(d.id, d.name);
    return out;
}

}  // namespace simqvfr
