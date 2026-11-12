#include "canvas.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>

#include <dwrite_3.h>

#include "font_resources.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace simqvfr::launcher::ui {
namespace {

D2D1_COLOR_F d2d(Color c) {
    return D2D1::ColorF(((c.rgb >> 16) & 0xff) / 255.0f, ((c.rgb >> 8) & 0xff) / 255.0f, (c.rgb & 0xff) / 255.0f, c.alpha);
}

// The bundled fonts' resources (launcher.rc).
constexpr int kFontResources[] = {IDR_FONT_INTER_REGULAR, IDR_FONT_INTER_MEDIUM, IDR_FONT_INTER_SEMIBOLD, IDR_FONT_INTER_BOLD,
                                  IDR_FONT_JOST_BOLD,     IDR_FONT_JOST_HEAVY,   IDR_FONT_JOST_BLACK};

// `family` in `fonts`, if it is there.
bool has_family(IDWriteFontCollection* fonts, const wchar_t* family) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    return fonts && SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

// The first installed family of a comma-separated list ("Segoe UI Variable Text, Segoe UI").
std::wstring first_installed(IDWriteFontCollection* system, const std::wstring& list) {
    std::wstring first;
    for (size_t at = 0; at <= list.size();) {
        size_t comma = list.find(L',', at);
        if (comma == std::wstring::npos)
            comma = list.size();
        std::wstring name = list.substr(at, comma - at);
        name.erase(0, name.find_first_not_of(L' '));
        name.erase(name.find_last_not_of(L' ') + 1);
        if (first.empty())
            first = name;
        if (!name.empty() && has_family(system, name.c_str()))
            return name;
        at = comma + 1;
    }
    return first;
}

}  // namespace

bool Canvas::initialize(HWND hwnd) {
    hwnd_ = hwnd;
    dpi_ = GetDpiForWindow(hwnd);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    load_fonts();
    const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(props, nullptr, 0, round_.GetAddressOf());
    create_target();
    return target_ != nullptr;
}

// Inter and Jost from the exe's resources into a font collection of their own (IDWriteFactory5, Windows 10 1703 and
// newer): the WOFF2 files are unpacked to OpenType first. A family not found there falls back to installed ones.
void Canvas::load_fonts() {
    ComPtr<IDWriteFactory5> factory;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (SUCCEEDED(dwrite_.As(&factory)) && SUCCEEDED(factory->CreateInMemoryFontFileLoader(loader.GetAddressOf())) &&
        SUCCEEDED(factory->RegisterFontFileLoader(loader.Get())) && SUCCEEDED(factory->CreateFontSetBuilder(builder.GetAddressOf()))) {
        const HMODULE module = GetModuleHandleW(nullptr);
        bool any = false;
        for (const int id : kFontResources) {
            const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
            const HGLOBAL memory = resource ? LoadResource(module, resource) : nullptr;
            const void* data = memory ? LockResource(memory) : nullptr;
            const DWORD size = resource ? SizeofResource(module, resource) : 0;
            if (!data || size < 4)
                continue;
            ComPtr<IDWriteFontFile> file;
            if (std::memcmp(data, "wOF2", 4) == 0) {
                ComPtr<IDWriteFontFileStream> stream;
                UINT64 unpacked = 0;
                const void* fragment = nullptr;
                void* context = nullptr;
                if (SUCCEEDED(factory->UnpackFontFile(DWRITE_CONTAINER_TYPE_WOFF2, data, size, stream.GetAddressOf())) &&
                    SUCCEEDED(stream->GetFileSize(&unpacked)) &&
                    SUCCEEDED(stream->ReadFileFragment(&fragment, 0, unpacked, &context))) {
                    // No owner: the loader keeps a copy of the bytes.
                    loader->CreateInMemoryFontFileReference(factory.Get(), fragment, static_cast<UINT32>(unpacked), nullptr,
                                                            file.GetAddressOf());
                    stream->ReleaseFileFragment(context);
                }
            } else {
                loader->CreateInMemoryFontFileReference(factory.Get(), data, size, nullptr, file.GetAddressOf());
            }
            any |= file && SUCCEEDED(builder->AddFontFile(file.Get()));
        }
        ComPtr<IDWriteFontSet> set;
        ComPtr<IDWriteFontCollection1> collection;
        if (any && SUCCEEDED(builder->CreateFontSet(set.GetAddressOf())) &&
            SUCCEEDED(factory->CreateFontCollectionFromFontSet(set.Get(), collection.GetAddressOf())))
            bundled_ = collection;
    }
    ComPtr<IDWriteFontCollection> system;
    dwrite_->GetSystemFontCollection(&system);
    for (size_t i = 0; i < std::size(tokens::kFamilies); ++i) {
        FamilyFont& f = families_[i];
        if (has_family(bundled_.Get(), tokens::kFamilies[i].name)) {
            f.name = tokens::kFamilies[i].name;
            f.collection = bundled_;
        } else {
            f.name = first_installed(system.Get(), tokens::kFamilies[i].fallback);
            f.collection = system;
        }
        // Its ascent and descent, from the face nearest its regular weight (Jost's Bold: no lighter one is bundled).
        UINT32 index = 0;
        BOOL exists = FALSE;
        ComPtr<IDWriteFontFamily> fontFamily;
        ComPtr<IDWriteFont> font;
        if (f.collection && SUCCEEDED(f.collection->FindFamilyName(f.name.c_str(), &index, &exists)) && exists &&
            SUCCEEDED(f.collection->GetFontFamily(index, &fontFamily)) &&
            SUCCEEDED(fontFamily->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                       DWRITE_FONT_STYLE_NORMAL, &font))) {
            DWRITE_FONT_METRICS m{};
            font->GetMetrics(&m);
            if (m.designUnitsPerEm > 0) {
                f.ascent = static_cast<float>(m.ascent) / m.designUnitsPerEm;
                f.descent = static_cast<float>(m.descent) / m.designUnitsPerEm;
            }
        }
    }
}

const Canvas::FamilyFont& Canvas::family(const tokens::TypeStyle& style) const {
    return families_[std::min(static_cast<size_t>(style.family), std::size(families_) - 1)];
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

void Canvas::fill(ID2D1Geometry* g, Color c) { target_->FillGeometry(g, brush(c)); }

void Canvas::stroke(ID2D1Geometry* g, Color c, float width) { target_->DrawGeometry(g, brush(c), width); }

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

void Canvas::image(const Rect& r, const std::vector<uint32_t>& pixels, int w, int h, uint64_t key) {
    if (!target_ || w <= 0 || h <= 0 || pixels.size() < static_cast<size_t>(w) * static_cast<size_t>(h))
        return;
    if (!image_ || imageKey_ != key || imageTarget_ != target_.Get()) {
        image_.Reset();
        const D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(target_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), pixels.data(),
                                         static_cast<UINT32>(w) * 4, props, image_.GetAddressOf())))
            return;
        imageKey_ = key;
        imageTarget_ = target_.Get();
    }
    target_->DrawBitmap(image_.Get(), r.d2d(), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

IDWriteTextFormat* Canvas::format(const tokens::TypeStyle& style, Align align, bool wrap) {
    const std::wstring key = std::to_wstring(style.size) + L"/" + std::to_wstring(style.lineHeight) + L"/" +
                             std::to_wstring(style.weight) + L"/" + std::to_wstring(static_cast<int>(style.family)) + L"/" +
                             std::to_wstring(static_cast<int>(align)) + (wrap ? L"w" : L"");
    auto it = formats_.find(key);
    if (it != formats_.end())
        return it->second.Get();
    const FamilyFont& fam = family(style);
    ComPtr<IDWriteTextFormat> f;
    dwrite_->CreateTextFormat(fam.name.c_str(), fam.collection.Get(), static_cast<DWRITE_FONT_WEIGHT>(style.weight),
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, style.size, L"en-us", &f);
    if (!f)
        return nullptr;
    f->SetTextAlignment(align == Align::Center     ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : align == Align::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                   : DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    // The style's line height, with the font's own box (ascent + descent) centred in it.
    const float box = (fam.ascent + fam.descent) * style.size;
    f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, style.lineHeight,
                      (style.lineHeight - box) / 2 + fam.ascent * style.size);
    formats_[key] = f;
    return f.Get();
}

// `s` laid out in `style`: upper-cased, letter-spaced and with tabular figures as the style asks.
ComPtr<IDWriteTextLayout> Canvas::layout(const std::wstring& s, const tokens::TypeStyle& style, Align align, bool wrap,
                                         float w, float h) {
    ComPtr<IDWriteTextLayout> out;
    IDWriteTextFormat* f = format(style, align, wrap);
    if (!f)
        return out;
    std::wstring text = s;
    if (style.upper)
        for (wchar_t& ch : text)
            ch = static_cast<wchar_t>(std::towupper(ch));
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), f, w, h, &out)))
        return nullptr;
    const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(text.size())};
    if (style.tabular) {
        ComPtr<IDWriteTypography> typography;
        if (SUCCEEDED(dwrite_->CreateTypography(&typography))) {
            typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
            out->SetTypography(typography.Get(), all);
        }
    }
    if (style.tracking != 0) {  // half before and half after each letter, so centred text stays centred
        ComPtr<IDWriteTextLayout1> spaced;
        const float half = style.tracking * style.size / 2;
        if (SUCCEEDED(out.As(&spaced)))
            spaced->SetCharacterSpacing(half, half, 0, all);
    }
    return out;
}

float Canvas::text(const std::wstring& s, const tokens::TypeStyle& style, const Rect& r, Color c, Align align, bool vcenter,
                   bool wrap) {
    if (s.empty())
        return 0;
    const ComPtr<IDWriteTextLayout> l = layout(s, style, align, wrap, r.w, r.h);
    if (!l)
        return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    const float y = vcenter ? r.y + (r.h - m.height) / 2 : r.y;
    target_->DrawTextLayout(D2D1::Point2F(r.x, y), l.Get(), brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    return m.widthIncludingTrailingWhitespace;
}

float Canvas::measure(const std::wstring& s, const tokens::TypeStyle& style) {
    const ComPtr<IDWriteTextLayout> l = layout(s, style, Align::Leading, false, 10000, 1000);
    if (!l)
        return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

float Canvas::measure_height(const std::wstring& s, const tokens::TypeStyle& style, float width) {
    const ComPtr<IDWriteTextLayout> l = layout(s, style, Align::Leading, true, width, 10000);
    if (!l)
        return style.lineHeight;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.height;
}

}  // namespace simqvfr::launcher::ui
