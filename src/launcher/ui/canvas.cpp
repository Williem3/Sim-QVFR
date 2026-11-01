#include "canvas.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace aceqv::launcher::ui {
namespace {

D2D1_COLOR_F d2d(Color c) {
    return D2D1::ColorF(((c.rgb >> 16) & 0xff) / 255.0f, ((c.rgb >> 8) & 0xff) / 255.0f, (c.rgb & 0xff) / 255.0f, c.alpha);
}

// The first installed family of a list (the kit's font stacks, Windows part).
std::wstring first_family(IDWriteFactory* dwrite, std::initializer_list<const wchar_t*> names) {
    ComPtr<IDWriteFontCollection> fonts;
    dwrite->GetSystemFontCollection(&fonts);
    for (const wchar_t* name : names) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (fonts && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists)
            return name;
    }
    return *names.begin();
}

}  // namespace

bool Canvas::initialize(HWND hwnd) {
    hwnd_ = hwnd;
    dpi_ = GetDpiForWindow(hwnd);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    systemFamily_ = first_family(dwrite_.Get(), {L"Segoe UI Variable Text", L"Segoe UI"});
    monoFamily_ = first_family(dwrite_.Get(), {L"Cascadia Mono", L"Consolas"});
    const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(props, nullptr, 0, round_.GetAddressOf());
    create_target();
    return target_ != nullptr;
}

void Canvas::create_target() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), static_cast<float>(dpi_), static_cast<float>(dpi_));
    factory_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
                                     target_.ReleaseAndGetAddressOf());
    brush_.Reset();
    if (target_) {
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        target_->CreateSolidColorBrush(D2D1::ColorF(0), brush_.GetAddressOf());
    }
}

void Canvas::resize() {
    if (!target_)
        return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    target_->Resize(D2D1::SizeU(rc.right, rc.bottom));
}

void Canvas::set_dpi(UINT dpi) {
    dpi_ = dpi;
    if (target_)
        target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    resize();
}

float Canvas::width() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    return rc.right * 96.0f / dpi_;
}

float Canvas::height() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    return rc.bottom * 96.0f / dpi_;
}

bool Canvas::begin() {
    if (!target_)
        create_target();
    if (!target_)
        return false;
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Canvas::end() {
    if (target_ && target_->EndDraw() == D2DERR_RECREATE_TARGET)
        target_.Reset();
}

ID2D1SolidColorBrush* Canvas::brush(Color c) {
    brush_->SetColor(d2d(c));
    return brush_.Get();
}

void Canvas::fill(const Rect& r, Color c, float radius) {
    if (radius > 0)
        target_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush(c));
    else
        target_->FillRectangle(r.d2d(), brush(c));
}

void Canvas::stroke(const Rect& r, Color c, float width, float radius) {
    // Strokes centre on the edge: inset by half the width so the line sits inside `r`, as CSS borders do.
    const Rect in = r.inset(width / 2, width / 2);
    const float rr = radius > 0 ? std::max(0.0f, radius - width / 2) : 0;
    if (rr > 0)
        target_->DrawRoundedRectangle(D2D1::RoundedRect(in.d2d(), rr, rr), brush(c), width);
    else
        target_->DrawRectangle(in.d2d(), brush(c), width);
}

void Canvas::line(float x0, float y0, float x1, float y1, Color c, float width) {
    target_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(c), width, round_.Get());
}

void Canvas::circle(float cx, float cy, float radius, Color c, bool filled, float width) {
    const D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
    if (filled)
        target_->FillEllipse(e, brush(c));
    else
        target_->DrawEllipse(e, brush(c), width);
}

void Canvas::control_shadow(const Rect& r, float radius, bool dark) {
    // 0 0.5px 1px rgba(0,0,0,0.14 light / 0.45 dark): approximated by a 0.5px offset, 1px larger halo.
    const float a = dark ? 0.45f : 0.14f;
    fill({r.x - 0.5f, r.y + 0.5f, r.w + 1, r.h + 0.5f}, {0, a * 0.5f}, radius + 0.5f);
    fill({r.x, r.y + 0.5f, r.w, r.h}, {0, a}, radius);
}

void Canvas::path(const char* d, const Rect& r, float box, Color c, float strokeWidth, bool filled) {
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory_->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink)))
        return;
    const float sx = r.w / box, sy = r.h / box;
    const auto pt = [&](float x, float y) { return D2D1::Point2F(r.x + x * sx, r.y + y * sy); };
    float x = 0, y = 0, startX = 0, startY = 0, lastCx = 0, lastCy = 0;
    bool open = false;
    char cmd = 0, prev = 0;
    const char* p = d;
    const auto number = [&](float& out) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ','))
            ++p;
        char* end = nullptr;
        out = std::strtof(p, &end);
        const bool ok = end != p;
        p = end;
        return ok;
    };
    const auto close_figure = [&] {
        if (open)
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        open = false;
    };
    while (*p) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ','))
            ++p;
        if (!*p)
            break;
        if (std::isalpha(static_cast<unsigned char>(*p)))
            cmd = *p++;
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        float a, b, c1, c2, e, f;
        if (up == 'Z') {
            if (open)
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            x = startX;
            y = startY;
            prev = 'Z';
            continue;
        }
        if (up == 'M') {
            if (!number(a) || !number(b))
                break;
            close_figure();
            x = rel ? x + a : a;
            y = rel ? y + b : b;
            startX = x;
            startY = y;
            sink->BeginFigure(pt(x, y), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            open = true;
            cmd = rel ? 'l' : 'L';  // further pairs are lines
        } else if (up == 'L') {
            if (!number(a) || !number(b))
                break;
            x = rel ? x + a : a;
            y = rel ? y + b : b;
            sink->AddLine(pt(x, y));
        } else if (up == 'H') {
            if (!number(a))
                break;
            x = rel ? x + a : a;
            sink->AddLine(pt(x, y));
        } else if (up == 'V') {
            if (!number(a))
                break;
            y = rel ? y + a : a;
            sink->AddLine(pt(x, y));
        } else if (up == 'C' || up == 'S') {
            if (up == 'C') {
                if (!number(a) || !number(b) || !number(c1) || !number(c2) || !number(e) || !number(f))
                    break;
                if (rel) {
                    a += x, b += y, c1 += x, c2 += y, e += x, f += y;
                }
            } else {
                if (!number(c1) || !number(c2) || !number(e) || !number(f))
                    break;
                if (rel) {
                    c1 += x, c2 += y, e += x, f += y;
                }
                const bool smooth = prev == 'C' || prev == 'S';
                a = smooth ? 2 * x - lastCx : x;
                b = smooth ? 2 * y - lastCy : y;
            }
            sink->AddBezier(D2D1::BezierSegment(pt(a, b), pt(c1, c2), pt(e, f)));
            lastCx = c1;
            lastCy = c2;
            x = e;
            y = f;
        } else {
            break;  // unsupported command
        }
        prev = up;
    }
    close_figure();
    sink->Close();
    if (filled)
        target_->FillGeometry(geometry.Get(), brush(c));
    else
        target_->DrawGeometry(geometry.Get(), brush(c), strokeWidth * sx, round_.Get());
}

void Canvas::push_clip(const Rect& r) { target_->PushAxisAlignedClip(r.d2d(), D2D1_ANTIALIAS_MODE_ALIASED); }
void Canvas::pop_clip() { target_->PopAxisAlignedClip(); }

IDWriteTextFormat* Canvas::format(const tokens::TypeStyle& style, Align align, bool wrap) {
    const std::wstring key = std::to_wstring(style.size) + L"/" + std::to_wstring(style.weight) + L"/" +
                             (style.mono ? L"m" : L"s") + std::to_wstring(static_cast<int>(align)) + (wrap ? L"w" : L"");
    auto it = formats_.find(key);
    if (it != formats_.end())
        return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    dwrite_->CreateTextFormat(style.mono ? monoFamily_.c_str() : systemFamily_.c_str(), nullptr,
                              static_cast<DWRITE_FONT_WEIGHT>(style.weight), DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, style.size, L"en-us", &f);
    if (!f)
        return nullptr;
    f->SetTextAlignment(align == Align::Center     ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : align == Align::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                   : DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    // The kit's line heights, baseline at 80% of the line (typical for Segoe UI).
    f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, style.lineHeight, style.lineHeight * 0.8f);
    formats_[key] = f;
    return f.Get();
}

float Canvas::text(const std::wstring& s, const tokens::TypeStyle& style, const Rect& r, Color c, Align align, bool vcenter,
                   bool wrap) {
    IDWriteTextFormat* f = format(style, align, wrap);
    if (!f || s.empty())
        return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, r.w, r.h, &layout)))
        return 0;
    if (style.mono) {
        // Tabular figures (the kit's `value` style).
        ComPtr<IDWriteTypography> typography;
        if (SUCCEEDED(dwrite_->CreateTypography(&typography))) {
            typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
            layout->SetTypography(typography.Get(), {0, static_cast<UINT32>(s.size())});
        }
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const float y = vcenter ? r.y + (r.h - m.height) / 2 : r.y;
    target_->DrawTextLayout(D2D1::Point2F(r.x, y), layout.Get(), brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    return m.widthIncludingTrailingWhitespace;
}

float Canvas::measure(const std::wstring& s, const tokens::TypeStyle& style) {
    IDWriteTextFormat* f = format(style, Align::Leading, false);
    ComPtr<IDWriteTextLayout> layout;
    if (!f || FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 10000, 1000, &layout)))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

float Canvas::measure_height(const std::wstring& s, const tokens::TypeStyle& style, float width) {
    IDWriteTextFormat* f = format(style, Align::Leading, true);
    ComPtr<IDWriteTextLayout> layout;
    if (!f || FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, width, 10000, &layout)))
        return style.lineHeight;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

}  // namespace aceqv::launcher::ui
