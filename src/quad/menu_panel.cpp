#include "menu_panel.h"

#include <algorithm>
#include <cstring>

#include "../log.h"

namespace simqvfr {
namespace {

// sRGB colours (the swapchain is _SRGB, so these bytes display as written). 0xRRGGBB.
constexpr uint32_t kBackground = 0x16181c, kHighlight = 0x0a6e78, kLabel = 0xe8eaed, kDim = 0x9aa0a6,
                   kCapped = 0xff6b5e;  // a value that did not take, and why
constexpr uint8_t kAlpha = 0xe6;
constexpr int kCornerRadius = 32;  // as the overlay's card
constexpr int kMargin = 36, kLineHeight = kMenuLineHeight, kTitleHeight = 64;
constexpr int kTabsHeight = 48, kRowHeight = 36, kNoteHeight = 34;
constexpr uint32_t kHeading = 0x7fd4dc;  // a section's heading: the highlight's teal, lighter (readable on the panel)

// The rows above the list: title, tabs, the Launch max row (or not), the stats or subtitle, the page switch.
int list_top(bool resolutionShown) {
    return 16 + kTitleHeight + kTabsHeight + (resolutionShown ? kRowHeight : 0) + kRowHeight + 8 + kLineHeight;
}
// Where the notes end: the footer's top, or the hint's.
int notes_bottom(bool footer) { return kPanelHeight - (footer ? 96 : 56); }
int row_height(const MenuLine& line) { return line.header ? kMenuHeaderHeight : kLineHeight; }

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

int text_width(HDC dc, HFONT font, const std::wstring& s) {
    SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &size);
    return static_cast<int>(size.cx);
}

}  // namespace

int menu_list_height(bool resolutionShown, size_t notes, bool footer) {
    const int shown = static_cast<int>(std::min<size_t>(notes, 3));
    return notes_bottom(footer) - (shown ? 6 + shown * kNoteHeight : 0) - 6 - list_top(resolutionShown);
}

int menu_first_visible(const std::vector<MenuLine>& lines, int first, int heightPx) {
    const int n = static_cast<int>(lines.size());
    if (n <= 1)
        return 1;
    first = std::clamp(first, 1, n - 1);
    int selected = -1;
    for (int i = 1; i < n; ++i)
        if (lines[i].selected)
            selected = i;
    const auto fits = [&](int from, int to) {  // lines from..to (inclusive) in the height
        int h = 0;
        for (int i = from; i <= to; ++i)
            h += row_height(lines[i]);
        return h <= heightPx;
    };
    if (selected >= 1) {
        // Up past the top: the selected line first, with its section's heading if that is right above it.
        if (selected < first)
            first = selected > 1 && lines[selected - 1].header ? selected - 1 : selected;
        // Down past the bottom: as little scroll as shows it.
        while (first < selected && !fits(first, selected))
            ++first;
    }
    // No empty room at the end while lines above are hidden.
    while (first > 1 && fits(first - 1, n - 1))
        --first;
    return first;
}

std::vector<uint32_t> draw_menu_panel(const std::wstring& title, const MenuResolution& resolution,
                                      const MenuStats& stats, const std::vector<MenuLine>& lines,
                                      const std::vector<std::wstring>& notes, const std::wstring& hint,
                                      const std::wstring& subtitle, const std::wstring& footer,
                                      const std::wstring& frame, bool frameBad,
                                      const std::vector<std::wstring>& tabs, int tab, int first) {
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
    HFONT titleFont = font(38, FW_SEMIBOLD), lineFont = font(34, FW_NORMAL), smallFont = font(26, FW_NORMAL),
          tabFont = font(26, FW_SEMIBOLD), headingFont = font(24, FW_SEMIBOLD);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, kPanelWidth, kPanelHeight, kBackground);
    text(dc, titleFont, title, kMargin, 16, kPanelWidth - kMargin, 16 + kTitleHeight, kLabel, DT_LEFT);
    // The frame times, from the performance overlay, beside the title: a step's effect without turning to the overlay.
    if (!frame.empty()) {  // right-aligned, after the title (the longest, "QVFR (API path) settings", leaves ~480 px)
        SelectObject(dc, titleFont);
        SIZE size{};
        GetTextExtentPoint32W(dc, title.c_str(), static_cast<int>(title.size()), &size);
        text(dc, smallFont, frame, kMargin + static_cast<int>(size.cx) + 28, 16, kPanelWidth - kMargin, 16 + kTitleHeight,
             frameBad ? kCapped : kDim, DT_RIGHT | DT_END_ELLIPSIS);
    }
    int y = 16 + kTitleHeight;
    // The pages as tabs: the current one filled, as the launcher's tab rail marks where you are.
    {
        int x = kMargin - 14;
        for (size_t i = 0; i < tabs.size(); ++i) {
            const int w = text_width(dc, tabFont, tabs[i]) + 28;
            const bool on = static_cast<int>(i) == tab;
            if (on)
                fill(dc, x, y + 4, x + w, y + kTabsHeight - 8, kHighlight);
            text(dc, tabFont, tabs[i], x, y + 4, x + w, y + kTabsHeight - 8, on ? kLabel : kDim, DT_CENTER);
            x += w + 8;
        }
        if (!tabs.empty())
            fill(dc, kMargin - 14, y + kTabsHeight - 4, kPanelWidth - kMargin + 14, y + kTabsHeight - 2, kHighlight);
        y += kTabsHeight;
    }
    // ACE's render target: the most this launch allows and what renders now.
    if (resolution.shown) {
        text(dc, smallFont, L"Launch max  " + resolution.max, kMargin, y, kPanelWidth / 2, y + 36, kDim, DT_LEFT);
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
    // A setting's line: its label up to its value (the launcher's long names never run under it), the value right.
    const auto draw_line = [&](const MenuLine& line, int top) {
        if (line.selected)
            fill(dc, kMargin - 16, top, kPanelWidth - kMargin + 16, top + kLineHeight, kHighlight);
        const std::wstring value = line.selected ? L"◀  " + line.value + L"  ▶" : line.value;
        // The arrows come from a fallback font that GetTextExtentPoint32 measures narrower than it draws: room added.
        const int valueWidth = text_width(dc, lineFont, value) + (line.selected ? 24 : 0);
        const int valueLeft = std::max(kPanelWidth / 2 - 160, kPanelWidth - kMargin - valueWidth);
        text(dc, lineFont, line.label, kMargin, top, valueLeft - 16, top + kLineHeight, kLabel, DT_LEFT | DT_END_ELLIPSIS);
        text(dc, lineFont, value, valueLeft - 24, top, kPanelWidth - kMargin, top + kLineHeight, line.capped ? kCapped : kLabel,
             DT_RIGHT);
    };
    std::vector<std::wstring> red;
    if (resolution.capped)
        red.push_back(L"Asked for " + resolution.asked + L": more than the max");
    red.insert(red.end(), notes.begin(), notes.end());
    // The page switch, pinned; under it the list from `first`, as much as fits above the notes.
    if (!lines.empty()) {
        draw_line(lines[0], y);
        y += kLineHeight;
    }
    const int listBottom = y + menu_list_height(resolution.shown, red.size(), !footer.empty());
    const int from = std::clamp(first, 1, std::max(1, static_cast<int>(lines.size())));
    if (from > 1)  // lines hidden above
        text(dc, smallFont, L"▲", kPanelWidth - kMargin + 4, y, kPanelWidth - 6, y + 30, kDim, DT_CENTER);
    int last = from - 1;
    for (int i = from; i < static_cast<int>(lines.size()); ++i) {
        const MenuLine& line = lines[i];
        if (y + row_height(line) > listBottom)
            break;
        if (line.header) {  // the card's name, with a rule over it
            fill(dc, kMargin, y + 8, kPanelWidth - kMargin, y + 9, kDim);
            text(dc, headingFont, line.label, kMargin, y + 10, kPanelWidth - kMargin, y + kMenuHeaderHeight, kHeading, DT_LEFT);
        } else {
            draw_line(line, y);
        }
        y += row_height(line);
        last = i;
    }
    if (last < static_cast<int>(lines.size()) - 1)  // lines hidden below
        text(dc, smallFont, L"▼", kPanelWidth - kMargin + 4, listBottom - 30, kPanelWidth - 6, listBottom, kDim, DT_CENTER);
    y = listBottom + 6;
    for (size_t i = 0; i < red.size() && i < 3; ++i, y += kNoteHeight)
        text(dc, smallFont, red[i], kMargin, y, kPanelWidth - kMargin, y + kNoteHeight, kCapped, DT_LEFT | DT_END_ELLIPSIS);
    if (!footer.empty())  // the in-game hotkeys, over the menu's own hint
        text(dc, smallFont, footer, kMargin, kPanelHeight - 96, kPanelWidth - kMargin, kPanelHeight - 56, kDim,
             DT_LEFT | DT_END_ELLIPSIS);
    text(dc, smallFont, hint, kMargin, kPanelHeight - 56, kPanelWidth - kMargin, kPanelHeight - 16, kDim, DT_LEFT);
    GdiFlush();
    // GDI writes BGRx; the swapchain wants RGBA with the panel's alpha (rounded corners), premultiplied (osd.h).
    const auto* src = static_cast<const uint32_t*>(bits);
    for (int py = 0; py < kPanelHeight; ++py)
        for (int px = 0; px < kPanelWidth; ++px) {
            const size_t i = static_cast<size_t>(py) * kPanelWidth + px;
            const double coverage = panel_corner_coverage(px, py, kPanelWidth, kPanelHeight, kCornerRadius);
            out[i] = panel_rgba(src[i], static_cast<uint8_t>(kAlpha * coverage + 0.5));
        }
    SelectObject(dc, previous);
    for (HFONT f : {titleFont, lineFont, smallFont, tabFont, headingFont})
        DeleteObject(f);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return out;
}

namespace {

class PanelUploadD3D12 final : public PanelUpload {
public:
    ~PanelUploadD3D12() override { shutdown(); }
    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue, int width, int height);
    bool upload(void* image, const std::vector<uint32_t>& pixels) override {
        return copy(static_cast<ID3D12Resource*>(image), pixels);
    }

private:
    bool copy(ID3D12Resource* image, const std::vector<uint32_t>& pixels);
    void shutdown();
    bool wait();  // true when the last copy is done (bounded)
    bool waitLogged_ = false;
    int width_ = kPanelWidth, height_ = kPanelHeight;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Resource* buffer_ = nullptr;  // upload heap, persistently mapped
    void* mapped_ = nullptr;
    ID3D12CommandAllocator* allocator_ = nullptr;
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE event_ = nullptr;
    uint64_t value_ = 0;
};

// D3D11: UpdateSubresource on the immediate context, which keeps no pipeline state of the game's.
class PanelUploadD3D11 final : public PanelUpload {
public:
    PanelUploadD3D11(ID3D11Device* device, int width, int height) : width_(width), height_(height) {
        device->GetImmediateContext(&context_);
    }
    ~PanelUploadD3D11() override {
        if (context_)
            context_->Release();
    }
    bool upload(void* image, const std::vector<uint32_t>& pixels) override {
        if (!context_ || !image || pixels.size() != static_cast<size_t>(width_) * height_)
            return false;
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(width_), static_cast<UINT>(height_), 1};
        context_->UpdateSubresource(static_cast<ID3D11Texture2D*>(image), 0, &box, pixels.data(),
                                    static_cast<UINT>(width_) * 4, 0);
        return true;
    }

private:
    int width_, height_;
    ID3D11DeviceContext* context_ = nullptr;
};

}  // namespace

bool PanelUploadD3D12::initialize(ID3D12Device* device, ID3D12CommandQueue* queue, int width, int height) {
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
    list_->SetName(L"Sim-QVFR settings panel");
    return true;
}

bool PanelUploadD3D12::wait() {
    if (!fence_)
        return true;
    UINT64 done = fence_->GetCompletedValue();
    if (done < value_ && event_ && SUCCEEDED(fence_->SetEventOnCompletion(value_, event_)))
        WaitForSingleObject(event_, 1000);
    done = fence_->GetCompletedValue();
    return done != UINT64_MAX && done >= value_;  // UINT64_MAX: the device is removed
}

void PanelUploadD3D12::shutdown() {
    // The last copy reads the buffer: wait in steps (a hung GPU is removed by the driver's timeout, after
    // which releasing is fine); past the cap leak the objects rather than free them under the GPU.
    bool idle = true;
    if (fence_) {
        for (int step = 0; step < 30 && !wait(); ++step) {
            if (fence_->GetCompletedValue() == UINT64_MAX)
                break;
            if (step == 0)
                log_line("settings panel: shutdown waiting for the GPU to finish the last copy");
            if (!event_)
                Sleep(1000);
        }
        const UINT64 done = fence_->GetCompletedValue();
        idle = done == UINT64_MAX || done >= value_;
    }
    if (!idle) {
        log_line("settings panel: the GPU didn't finish in 30 s: its copy objects are leaked, not released");
        list_ = nullptr;
        allocator_ = nullptr;
        buffer_ = nullptr;
        fence_ = nullptr;
        event_ = nullptr;  // the fence may still signal it
    }
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
    value_ = 0;  // a new fence starts at 0
}

bool PanelUploadD3D12::copy(ID3D12Resource* image, const std::vector<uint32_t>& pixels) {
    if (!list_ || !image || pixels.size() != static_cast<size_t>(width_) * height_)
        return false;
    if (!wait()) {  // the previous copy may still read the buffer and the allocator: skip this one
        if (!waitLogged_)
            log_line("settings panel: the GPU hasn't finished the previous copy: upload skipped");
        waitLogged_ = true;
        return false;
    }
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


std::unique_ptr<PanelUpload> make_panel_upload(const SessionGraphics& graphics, int width, int height) {
    if (graphics.d3d12 && graphics.queue) {
        auto upload = std::make_unique<PanelUploadD3D12>();
        return upload->initialize(graphics.d3d12, graphics.queue, width, height) ? std::move(upload) : nullptr;
    }
    if (graphics.d3d11 && width > 0 && height > 0)
        return std::make_unique<PanelUploadD3D11>(graphics.d3d11, width, height);
    return nullptr;
}

}  // namespace simqvfr
