// The performance overlay's card (osd.h), drawn with GDI like the settings panel (menu_panel.cpp), in
// macOS's dark HUD style: a rounded translucent card, grey labels, white values and the system colours
// for state.
#include <algorithm>
#include <cmath>

#include <windows.h>

#include "osd.h"

namespace aceqv {
namespace {

// sRGB colours, 0xRRGGBB (the swapchain is _SRGB).
constexpr uint32_t kCard = 0x1c1c1e, kPrimary = 0xf5f5f7, kSecondary = 0x98989d, kSeparator = 0x38383a,
                   kTrack = 0x2c2c2e, kGreen = 0x30d158, kYellow = 0xffd60a, kRed = 0xff453a;
constexpr uint8_t kCardAlpha = 0xd9;
constexpr int kRadius = 32, kMargin = 36;

COLORREF gdi(uint32_t rgb) { return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

uint32_t mix(uint32_t a, uint32_t b, double t) {  // a towards b by t
    uint32_t out = 0;
    for (int shift : {0, 8, 16}) {
        const double x = ((a >> shift) & 0xff) * (1 - t) + ((b >> shift) & 0xff) * t;
        out |= static_cast<uint32_t>(std::lround(x)) << shift;
    }
    return out;
}

// Numbers stay white unless something needs a look; badges show every state.
uint32_t tone_colour(OsdTone tone, bool pill) {
    switch (tone) {
    case OsdTone::Good: return pill ? kGreen : kPrimary;
    case OsdTone::Warn: return kYellow;
    case OsdTone::Bad: return kRed;
    default: return kPrimary;
    }
}

void fill(HDC dc, int left, int top, int right, int bottom, uint32_t rgb) {
    const RECT r{left, top, right, bottom};
    HBRUSH brush = CreateSolidBrush(gdi(rgb));
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

void rounded(HDC dc, int left, int top, int right, int bottom, int radius, uint32_t rgb) {
    HBRUSH brush = CreateSolidBrush(gdi(rgb));
    HPEN pen = CreatePen(PS_SOLID, 1, gdi(rgb));
    HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, left, top, right, bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

int text(HDC dc, HFONT font, const std::wstring& s, int left, int top, int right, int bottom, uint32_t rgb, UINT align) {
    SelectObject(dc, font);
    SetTextColor(dc, gdi(rgb));
    RECT r{left, top, right, bottom};
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SIZE size{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &size);
    return size.cx;
}

// The card's alpha at a pixel: kCardAlpha inside the rounded rectangle, antialiased at its corners.
uint8_t card_alpha(int x, int y) {
    const double cx = std::clamp(x + 0.5, static_cast<double>(kRadius), static_cast<double>(kOsdWidth - kRadius));
    const double cy = std::clamp(y + 0.5, static_cast<double>(kRadius), static_cast<double>(kOsdHeight - kRadius));
    const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
    const double coverage = std::clamp(kRadius + 0.5 - d, 0.0, 1.0);
    return static_cast<uint8_t>(std::lround(kCardAlpha * coverage));
}

}  // namespace

std::vector<uint32_t> draw_osd_panel(const OsdSummary& summary, const std::vector<double>& history,
                                     const std::vector<OsdTile>& tiles, const std::vector<OsdRow>& rows) {
    std::vector<uint32_t> out(static_cast<size_t>(kOsdWidth) * kOsdHeight, 0);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kOsdWidth;
    bi.bmiHeader.biHeight = -kOsdHeight;  // top-down
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
    const auto font = [](int height, int weight) {  // grey-scale antialiasing, as the settings panel
        return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    HFONT labelFont = font(21, FW_NORMAL), bigFont = font(58, FW_SEMIBOLD), unitFont = font(24, FW_NORMAL),
          detailFont = font(21, FW_NORMAL), valueFont = font(25, FW_SEMIBOLD);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, kOsdWidth, kOsdHeight, kCard);

    // The tiles: a grey label, a large number with its unit, and a coloured line under it; hairlines
    // between them.
    const int tileTop = 24, tileWidth = (kOsdWidth - 2 * kMargin) / std::max<int>(1, static_cast<int>(tiles.size()));
    for (size_t i = 0; i < tiles.size(); ++i) {
        const OsdTile& tile = tiles[i];
        const int left = kMargin + static_cast<int>(i) * tileWidth + (i ? 24 : 0), right = kMargin + (static_cast<int>(i) + 1) * tileWidth;
        if (i)
            fill(dc, left - 24, tileTop + 8, left - 23, tileTop + 120, kSeparator);
        text(dc, labelFont, tile.label, left, tileTop, right, tileTop + 26, kSecondary, DT_LEFT);
        const uint32_t valueColour = tile.tone == OsdTone::Bad ? kRed : kPrimary;
        const int valueWidth = text(dc, bigFont, tile.value, left, tileTop + 26, right, tileTop + 98, valueColour, DT_LEFT);
        if (!tile.unit.empty())
            text(dc, unitFont, tile.unit, left + valueWidth + 6, tileTop + 50, right, tileTop + 92, kSecondary, DT_LEFT);
        const uint32_t detailColour = tile.tone == OsdTone::Neutral ? kSecondary : tone_colour(tile.tone, true);
        text(dc, detailFont, tile.detail, left, tileTop + 98, right, tileTop + 126, detailColour, DT_LEFT);
    }

    // Frame times, newest on the right: one bar per frame, coloured by how it met the budget, and the
    // budget as a line. The scale is two budgets (or the slowest frame).
    const int graphTop = tileTop + 142, graphBottom = graphTop + 60, graphLeft = kMargin, graphRight = kOsdWidth - kMargin;
    rounded(dc, graphLeft, graphTop, graphRight, graphBottom, 14, kTrack);
    if (!history.empty()) {
        const double slowest = *std::max_element(history.begin(), history.end());
        const double scale = std::max(summary.budgetMs > 0 ? 2 * summary.budgetMs : 0.0, slowest);
        const int inner = graphRight - graphLeft - 16, height = graphBottom - graphTop - 12;
        const size_t n = history.size();
        for (size_t i = 0; i < n && scale > 0; ++i) {
            const int left = graphLeft + 8 + static_cast<int>(inner * i / n);
            const int right = std::max(left + 1, graphLeft + 8 + static_cast<int>(inner * (i + 1) / n) - 1);
            const int h = std::max(2, static_cast<int>(height * std::min(1.0, history[i] / scale)));
            const double ratio = summary.budgetMs > 0 ? history[i] / summary.budgetMs : 1;
            const uint32_t colour = ratio <= 1.05 ? kGreen : ratio <= 1.5 ? kYellow : kRed;
            fill(dc, left, graphBottom - 6 - h, right, graphBottom - 6, mix(kTrack, colour, 0.8));
        }
        if (summary.budgetMs > 0 && scale > 0) {
            const int line = graphBottom - 6 - static_cast<int>(height * summary.budgetMs / scale);
            fill(dc, graphLeft + 8, line, graphRight - 8, line + 2, mix(kTrack, kPrimary, 0.45));
        }
    }

    // The cells: three columns, a grey label over a white value (or a badge for a state, and a meter
    // beside VRAM's).
    const int cellTop = graphBottom + 18, cellHeight = 76, cellWidth = (kOsdWidth - 2 * kMargin) / 3;
    for (size_t i = 0; i < rows.size(); ++i) {
        const OsdRow& row = rows[i];
        const int left = kMargin + static_cast<int>(i % 3) * cellWidth, right = left + cellWidth - 24;
        const int top = cellTop + static_cast<int>(i / 3) * cellHeight;
        text(dc, labelFont, row.label, left, top, right, top + 26, kSecondary, DT_LEFT);
        const int valueTop = top + 28, valueBottom = top + 64;
        const uint32_t colour = tone_colour(row.tone, row.pill);
        if (row.pill) {
            SelectObject(dc, valueFont);
            SIZE size{};
            GetTextExtentPoint32W(dc, row.value.c_str(), static_cast<int>(row.value.size()), &size);
            const int pillRight = std::min(right, left + static_cast<int>(size.cx) + 28);
            rounded(dc, left, valueTop + 2, pillRight, valueBottom - 2, valueBottom - valueTop - 4,
                    row.tone == OsdTone::Neutral ? kTrack : mix(kCard, colour, 0.22));
            text(dc, valueFont, row.value, left + 14, valueTop, pillRight - 14, valueBottom,
                 row.tone == OsdTone::Neutral ? kSecondary : colour, DT_LEFT);
        } else {
            const int valueWidth = text(dc, valueFont, row.value, left, valueTop, right, valueBottom,
                                        row.tone == OsdTone::Neutral ? kPrimary : colour, DT_LEFT);
            if (row.meter >= 0) {  // after the value
                const int meterLeft = left + valueWidth + 16;
                if (right - meterLeft > 40) {
                    const int mid = (valueTop + valueBottom) / 2;
                    rounded(dc, meterLeft, mid - 5, right, mid + 5, 10, kTrack);
                    rounded(dc, meterLeft, mid - 5, meterLeft + std::max(10, static_cast<int>((right - meterLeft) * row.meter)),
                            mid + 5, 10, tone_colour(row.tone, true));
                }
            }
        }
    }
    GdiFlush();
    // GDI writes BGRx; the swapchain wants RGBA with the card's alpha.
    const auto* src = static_cast<const uint32_t*>(bits);
    for (int py = 0; py < kOsdHeight; ++py)
        for (int px = 0; px < kOsdWidth; ++px) {
            const size_t i = static_cast<size_t>(py) * kOsdWidth + px;
            const uint32_t p = src[i];
            out[i] = ((p >> 16) & 0xff) | (p & 0xff00) | ((p & 0xff) << 16) | (static_cast<uint32_t>(card_alpha(px, py)) << 24);
        }
    SelectObject(dc, previous);
    for (HFONT f : {labelFont, bigFont, unitFont, detailFont, valueFont})
        DeleteObject(f);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return out;
}

std::vector<uint32_t> draw_notice_panel(const std::wstring& title, const std::vector<std::wstring>& lines) {
    std::vector<uint32_t> out(static_cast<size_t>(kOsdWidth) * kOsdHeight, 0);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kOsdWidth;
    bi.bmiHeader.biHeight = -kOsdHeight;  // top-down
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
    const auto font = [](int height, int weight) {
        return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    HFONT titleFont = font(44, FW_SEMIBOLD), bodyFont = font(28, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, kOsdWidth, kOsdHeight, kCard);
    // A yellow bar down the left edge, the title beside it, then each paragraph wrapped.
    rounded(dc, kMargin, 44, kMargin + 8, 100, 8, kYellow);
    text(dc, titleFont, title, kMargin + 28, 40, kOsdWidth - kMargin, 104, kYellow, DT_LEFT);
    int y = 132;
    SelectObject(dc, bodyFont);
    for (size_t i = 0; i < lines.size() && y < kOsdHeight - kMargin; ++i) {
        SetTextColor(dc, gdi(i == 0 ? kPrimary : kSecondary));
        RECT r{kMargin + 28, y, kOsdWidth - kMargin, kOsdHeight - kMargin};
        y += DrawTextW(dc, lines[i].c_str(), static_cast<int>(lines[i].size()), &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX) + 18;
    }
    GdiFlush();
    const auto* src = static_cast<const uint32_t*>(bits);
    for (int py = 0; py < kOsdHeight; ++py)
        for (int px = 0; px < kOsdWidth; ++px) {
            const size_t i = static_cast<size_t>(py) * kOsdWidth + px;
            const uint32_t p = src[i];
            out[i] = ((p >> 16) & 0xff) | (p & 0xff00) | ((p & 0xff) << 16) | (static_cast<uint32_t>(card_alpha(px, py)) << 24);
        }
    SelectObject(dc, previous);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return out;
}

}  // namespace aceqv
