// Drawing for the launcher window: Direct2D shapes and DirectWrite text in DIPs (px at 96 DPI), with the design
// tokens' colours and type styles (theme_tokens.h) in the bundled fonts (font_resources.h). One Canvas per window.
#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "theme_tokens.h"

namespace simqvfr::launcher::ui {

using Microsoft::WRL::ComPtr;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    D2D1_RECT_F d2d() const { return D2D1::RectF(x, y, x + w, y + h); }
};

enum class Align { Leading, Center, Trailing };

// A colour as 0xRRGGBB plus alpha.
struct Color {
    uint32_t rgb = 0;
    float alpha = 1;
};

class Canvas {
public:
    bool initialize(HWND hwnd);
    // Call on WM_SIZE / WM_DPICHANGED.
    void resize();
    void set_dpi(UINT dpi);
    float dpi_scale() const { return dpi_ / 96.0f; }
    // Size of the client area in DIPs.
    float width() const;
    float height() const;

    bool begin();
    void end();  // recreates the target on the next frame if the device was lost

    void fill(const Rect& r, Color c, float radius = 0);
    void stroke(const Rect& r, Color c, float width, float radius = 0);
    void line(float x0, float y0, float x1, float y1, Color c, float width);
    void circle(float cx, float cy, float radius, Color c, bool filled, float width = 1);
    // Any Direct2D geometry (from factory()), filled, or stroked centred on its edge.
    void fill(ID2D1Geometry* g, Color c);
    void stroke(ID2D1Geometry* g, Color c, float width);
    // An SVG path ('d' attribute: M L H V C S Z, absolute or relative) scaled from a `box`-sized
    // viewBox into `r`, stroked (round caps and joins) or filled.
    void path(const char* d, const Rect& r, float box, Color c, float strokeWidth, bool filled = false);
    void push_clip(const Rect& r);
    void pop_clip();
    // A `w` x `h` image of premultiplied BGRA pixels (0xAARRGGBB), scaled into `r`. Kept as a bitmap while `key` stays
    // the same (and the target lives): pass a new key when the pixels change.
    void image(const Rect& r, const std::vector<uint32_t>& pixels, int w, int h, uint64_t key);

    // Text in a type style (its family, weight, tabular figures, uppercase and letter spacing). Returns the drawn width.
    float text(const std::wstring& s, const tokens::TypeStyle& style, const Rect& r, Color c, Align align = Align::Leading,
               bool vcenter = true, bool wrap = false);
    float measure(const std::wstring& s, const tokens::TypeStyle& style);
    // Height of wrapped text at a width.
    float measure_height(const std::wstring& s, const tokens::TypeStyle& style, float width);

    ID2D1RenderTarget* target() { return target_.Get(); }
    ID2D1Factory* factory() { return factory_.Get(); }
    // Whether the bundled fonts loaded (else the families' installed fallbacks draw the text).
    bool bundled_fonts() const { return bundled_ != nullptr; }

private:
    // A type style's family as drawn: its name, the collection it is in (null: the system's), and its ascent and
    // descent as shares of the size (to centre the line box on the font's own box).
    struct FamilyFont {
        std::wstring name;
        ComPtr<IDWriteFontCollection> collection;
        float ascent = 0.8f, descent = 0.2f;
    };
    void load_fonts();
    const FamilyFont& family(const tokens::TypeStyle& style) const;
    IDWriteTextFormat* format(const tokens::TypeStyle& style, Align align, bool wrap);
    ComPtr<IDWriteTextLayout> layout(const std::wstring& s, const tokens::TypeStyle& style, Align align, bool wrap, float w,
                                     float h);
    ID2D1SolidColorBrush* brush(Color c);
    void create_target();

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> round_;
    ComPtr<ID2D1Bitmap> image_;
    uint64_t imageKey_ = 0;
    ID2D1RenderTarget* imageTarget_ = nullptr;  // the target image_ was made for
    std::map<std::wstring, ComPtr<IDWriteTextFormat>> formats_;
    ComPtr<IDWriteFontCollection> bundled_;  // Inter and Jost, from the exe's resources
    FamilyFont families_[std::size(tokens::kFamilies)];
};

}  // namespace simqvfr::launcher::ui
