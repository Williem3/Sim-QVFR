#include "menu_panel.h"

#include <algorithm>
#include <cstring>

#include "../log.h"

namespace aceqv {
namespace {

// sRGB colours (the swapchain is _SRGB, so these bytes display as written). 0xRRGGBB.
constexpr uint32_t kBackground = 0x16181c, kHighlight = 0x0a6e78, kLabel = 0xe8eaed, kDim = 0x9aa0a6,
                   kCapped = 0xff6b5e;  // a value that did not take, and why
constexpr uint8_t kAlpha = 0xe6;
constexpr int kMargin = 36, kLineHeight = 50, kTitleHeight = 64;

COLORREF gdi(uint32_t rgb) { return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

void fill(HDC dc, int left, int top, int right, int bottom, uint32_t rgb) {
    const RECT r{left, top, right, bottom};
    HBRUSH brush = CreateSolidBrush(gdi(rgb));
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

void text(HDC dc, HFONT font, const std::wstring& s, int left, int top, int right, int bottom, uint32_t rgb, UINT align) {
    SelectObject(dc, font);
    SetTextColor(dc, gdi(rgb));
    RECT r{left, top, right, bottom};
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

}  // namespace

std::vector<uint32_t> draw_menu_panel(const std::wstring& title, const MenuResolution& resolution,
                                      const MenuStats& stats, const std::vector<MenuLine>& lines,
                                      const std::vector<std::wstring>& notes, const std::wstring& hint,
                                      const std::wstring& subtitle, const std::wstring& footer) {
    std::vector<uint32_t> out(static_cast<size_t>(kPanelWidth) * kPanelHeight, 0);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kPanelWidth;
    bi.bmiHeader.biHeight = -kPanelHeight;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bitmap || !bits) {
        if (dc)
            DeleteDC(dc);
        return out;
    }
    HGDIOBJ previous = SelectObject(dc, bitmap);
    // Grey-scale antialiasing: ClearType's colour fringes would show on a panel seen through lenses.
    const auto font = [](int height, int weight) {
        return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    HFONT titleFont = font(38, FW_SEMIBOLD), lineFont = font(34, FW_NORMAL), smallFont = font(26, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, kPanelWidth, kPanelHeight, kBackground);
    text(dc, titleFont, title, kMargin, 16, kPanelWidth - kMargin, 16 + kTitleHeight, kLabel, DT_LEFT);
    int y = 16 + kTitleHeight;
    // ACE's render target: the most this launch allows and what renders now.
    if (resolution.shown) {
        text(dc, smallFont, L"Max  " + resolution.max, kMargin, y, kPanelWidth / 2, y + 36, kDim, DT_LEFT);
        text(dc, smallFont, L"Now  " + resolution.current, kPanelWidth / 2, y, kPanelWidth - kMargin, y + 36, kLabel,
             DT_RIGHT);
        y += 36;
    }
    if (!subtitle.empty()) {  // the Foveated DLSS page: Cheeky's credit line
        text(dc, smallFont, subtitle, kMargin, y, kPanelWidth - kMargin, y + 36, kDim, DT_LEFT);
    } else {
        // The focus area's sharpness at its centre and what the foveation saves.
        const std::wstring ppd = stats.centerPpd > 0 ? std::to_wstring(stats.centerPpd) : L"\u2014";
        const std::wstring saved = stats.savedPercent >= 0 ? std::to_wstring(stats.savedPercent) + L"%"
                                                           : L"None (" + std::to_wstring(-stats.savedPercent) + L"% more)";
        text(dc, smallFont, L"Center PPD  " + ppd, kMargin, y, kPanelWidth / 2, y + 36, kLabel, DT_LEFT);
        text(dc, smallFont, L"Pixels saved  " + saved, kPanelWidth / 2, y, kPanelWidth - kMargin, y + 36, kLabel,
             DT_RIGHT);
    }
    y += 36 + 8;
    for (const MenuLine& line : lines) {
        if (line.selected)
            fill(dc, kMargin - 16, y, kPanelWidth - kMargin + 16, y + kLineHeight, kHighlight);
        text(dc, lineFont, line.label, kMargin, y, kPanelWidth / 2 + 120, y + kLineHeight, kLabel, DT_LEFT);
        const std::wstring value = line.selected ? L"◀  " + line.value + L"  ▶" : line.value;
        text(dc, lineFont, value, kPanelWidth / 2 - 120, y, kPanelWidth - kMargin, y + kLineHeight,
             line.capped ? kCapped : kLabel, DT_RIGHT);
        y += kLineHeight;
    }
    y += 6;
    std::vector<std::wstring> red;
    if (resolution.capped)
        red.push_back(L"Asked for " + resolution.asked + L": more than the max");
    red.insert(red.end(), notes.begin(), notes.end());
    for (size_t i = 0; i < red.size() && i < 3; ++i, y += 34)
        text(dc, smallFont, red[i], kMargin, y, kPanelWidth - kMargin, y + 34, kCapped, DT_LEFT | DT_END_ELLIPSIS);
    if (!footer.empty())  // the in-game hotkeys, over the menu's own hint
        text(dc, smallFont, footer, kMargin, kPanelHeight - 96, kPanelWidth - kMargin, kPanelHeight - 56, kDim,
             DT_LEFT | DT_END_ELLIPSIS);
    text(dc, smallFont, hint, kMargin, kPanelHeight - 56, kPanelWidth - kMargin, kPanelHeight - 16, kDim, DT_LEFT);
    GdiFlush();
    // GDI writes BGRx; the swapchain wants RGBA with the panel's alpha.
    const auto* src = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < out.size(); ++i) {
        const uint32_t p = src[i];
        out[i] = ((p >> 16) & 0xff) | (p & 0xff00) | ((p & 0xff) << 16) | (static_cast<uint32_t>(kAlpha) << 24);
    }
    SelectObject(dc, previous);
    for (HFONT f : {titleFont, lineFont, smallFont})
        DeleteObject(f);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return out;
}

bool PanelUpload::initialize(ID3D12Device* device, ID3D12CommandQueue* queue, int width, int height) {
    shutdown();
    if (width <= 0 || height <= 0 || (width * 4) % 256 != 0)
        return false;
    width_ = width;
    height_ = height;
    device_ = device;
    queue_ = queue;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(width_) * 4 * height_;  // row pitch width x 4: 256-aligned
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&buffer_)));
    const D3D12_RANGE none{0, 0};
    ok = ok && SUCCEEDED(buffer_->Map(0, &none, &mapped_));
    ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))) &&
         SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_, nullptr, IID_PPV_ARGS(&list_))) &&
         SUCCEEDED(list_->Close()) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ok || !event_) {
        log_line("settings panel: D3D12 setup failed");
        shutdown();
        return false;
    }
    list_->SetName(L"ace-quadview settings panel");
    return true;
}

void PanelUpload::wait() {
    if (fence_ && event_ && fence_->GetCompletedValue() < value_ && SUCCEEDED(fence_->SetEventOnCompletion(value_, event_)))
        WaitForSingleObject(event_, 1000);
}

void PanelUpload::shutdown() {
    wait();  // the last copy reads the buffer
    if (list_)
        list_->Release();
    if (allocator_)
        allocator_->Release();
    if (buffer_)
        buffer_->Release();
    if (fence_)
        fence_->Release();
    if (event_)
        CloseHandle(event_);
    list_ = nullptr;
    allocator_ = nullptr;
    buffer_ = nullptr;
    mapped_ = nullptr;
    fence_ = nullptr;
    event_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
}

bool PanelUpload::upload(ID3D12Resource* image, const std::vector<uint32_t>& pixels) {
    if (!list_ || !image || pixels.size() != static_cast<size_t>(width_) * height_)
        return false;
    wait();  // the previous copy is done with the buffer and the allocator
    std::memcpy(mapped_, pixels.data(), pixels.size() * 4);
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_, nullptr)))
        return false;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = image;
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list_->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{buffer_, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint = {image->GetDesc().Format, static_cast<UINT>(width_), static_cast<UINT>(height_), 1,
                                     static_cast<UINT>(width_) * 4};
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list_->ResourceBarrier(1, &barrier);
    if (FAILED(list_->Close()))
        return false;
    ID3D12CommandList* lists[] = {list_};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_, ++value_);
    return true;
}

}  // namespace aceqv
