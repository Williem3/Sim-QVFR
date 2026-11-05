// The in-headset settings panel (settings_menu.h): its image drawn with GDI into a bitmap (no font
// of our own), then copied into an OpenXR swapchain image on the game's graphics API (D3D12: its queue;
// D3D11: its immediate context). The swapchain is submitted as a quad layer (xr_layer.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "session_graphics.h"

#include "settings_menu.h"

namespace simqvfr {

inline constexpr int kPanelWidth = 1024, kPanelHeight = 1072;  // up to 13 lines, 3 notes, the footer and the hint

// RGBA8 pixels (sRGB-encoded, straight alpha), kPanelWidth x kPanelHeight, rows top to bottom.
// `title`, then ACE's render target (max and now, QVFR), the centre PPD and pixels saved (or, when
// `subtitle` is set, that line instead: the Foveated DLSS page's credit), the lines
// (capped values in red), then up to three red lines (what the values ask for when it is more than
// the max, and `notes`: why and what to lower), `footer` (the in-game hotkeys; empty: none) and `hint` at the bottom.
std::vector<uint32_t> draw_menu_panel(const std::wstring& title, const MenuResolution& resolution,
                                      const MenuStats& stats, const std::vector<MenuLine>& lines,
                                      const std::vector<std::wstring>& notes, const std::wstring& hint,
                                      const std::wstring& subtitle = L"", const std::wstring& footer = L"");

// Copies a panel image (RGBA8, width x height) into a swapchain image of the session's API. D3D12: on the game's
// queue, the image kept in D3D12_RESOURCE_STATE_RENDER_TARGET around the copy, as OpenXR requires for a colour
// swapchain; `width` x 4 bytes must be a multiple of 256 (the copy's row pitch): widths in multiples of 64.
// D3D11: on the device's immediate context, from the thread that submits frames.
class PanelUpload {
public:
    virtual ~PanelUpload() = default;
    // `image`: an ID3D12Resource* or ID3D11Texture2D* (swapchain_textures).
    virtual bool upload(void* image, const std::vector<uint32_t>& pixels) = 0;
};
// Null if `graphics` has no device or the setup fails (logged).
std::unique_ptr<PanelUpload> make_panel_upload(const SessionGraphics& graphics, int width = kPanelWidth,
                                               int height = kPanelHeight);

}  // namespace simqvfr
