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

inline constexpr int kPanelWidth = 1024, kPanelHeight = 1072;

// The list's rows: a setting's line, and a section's heading (the launcher's card name).
inline constexpr int kMenuLineHeight = 50, kMenuHeaderHeight = 40;

// The list's height in pixels under the pinned page switch, with the Launch max row or not, `notes` red lines (at most
// three drawn) and the hotkeys' footer or not: what the rest of the panel leaves.
int menu_list_height(bool resolutionShown, size_t notes, bool footer);

// The first line drawn under the page switch (lines[0], always drawn), so the selected line shows in `heightPx`:
// `first` (the last draw's) moved only as far as it must, a section's heading kept with the line under it, and no
// room left empty at the end while lines above are hidden. At least 1.
int menu_first_visible(const std::vector<MenuLine>& lines, int first, int heightPx);

// RGBA8 pixels (sRGB-encoded, premultiplied alpha: osd.h's panel_rgba), kPanelWidth x kPanelHeight, rows top to
// bottom, a translucent panel with rounded corners.
// `title`, then ACE's render target (max and now, QVFR), the centre PPD and pixels saved (or, when
// `subtitle` is set, that line instead: the Foveated DLSS page's credit), the lines
// (capped values in red), then up to three red lines (what the values ask for when it is more than
// the max, and `notes`: why and what to lower), `footer` (the in-game hotkeys; empty: none) and `hint` at the bottom.
// Under the title, `tabs` (the pages' names, menu_tabs) as a strip with `tab` highlighted; the list from `first` on
// (menu_first_visible), arrows at its right edge where lines are hidden above or below.
std::vector<uint32_t> draw_menu_panel(const std::wstring& title, const MenuResolution& resolution,
                                      const MenuStats& stats, const std::vector<MenuLine>& lines,
                                      const std::vector<std::wstring>& notes, const std::wstring& hint,
                                      const std::wstring& subtitle = L"", const std::wstring& footer = L"",
                                      const std::wstring& frame = L"", bool frameBad = false,
                                      const std::vector<std::wstring>& tabs = {}, int tab = 0, int first = 1);

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
