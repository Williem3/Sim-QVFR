// The helmet's pieces (helmet.h) as both compositors keep them: each PNG decoded and looked at once (where its
// opaque part ends, whether it is see-through, its white point, the rows it draws on), then handed to the
// compositor as a premultiplied sRGB RGBA8 image for its own texture. So the D3D12 compositor (ACE) and the
// D3D11 one (ACC) load, measure and band the same pieces the same way; each keeps only its textures.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace simqvfr {

// The helmet's pieces (helmet.h), drawn over the scene in this order.
enum class HelmetPiece { Visor, Bottom, Top };

// What a loaded piece is like (from its pixels).
struct HelmetPieceInfo {
    double aspect = 1;         // height per width
    double edge = -1;          // where its opaque part ends down its middle column, in image heights (-1: none)
    bool opaque = false;       // (almost) no transparency
    double whitePoint = 1;     // the linear brightness of its brightest 1%
    std::vector<bool> rows, tintRows;  // kHelmetRows bins: where it draws; where, as glass, it tints
};

// Decodes `path` (a PNG with alpha, through WIC) and looks at it as piece `which`; `pixels` is then its image,
// premultiplied (so filtering never blends in the colour of transparent pixels), sRGB-encoded RGBA8, `width` x
// `height`. False if it cannot be read.
bool decode_helmet_piece(HelmetPiece which, const std::wstring& path, std::vector<uint8_t>& pixels, uint32_t& width,
                         uint32_t& height, HelmetPieceInfo& info);

class HelmetPieceSet {
public:
    virtual ~HelmetPieceSet() = default;
    // A helmet piece's image (a PNG with alpha), loaded into the compositor's texture; an empty path, or one that
    // cannot be read, drops it. False if it cannot be read (or the compositor isn't ready).
    bool load_helmet_piece(HelmetPiece piece, const std::wstring& path);
    bool has_helmet_pieces() const { return loaded_[0] || loaded_[1] || loaded_[2]; }
    // The loaded pieces as the shader takes them: 1 << HelmetPiece.
    unsigned helmet_piece_mask() const;
    // The first loaded piece's height per width (1 without one), and where a piece's opaque part ends down
    // its middle column, in image heights: the top piece's from its top, the bottom piece's from its bottom
    // (-1: not loaded, or not opaque there).
    double helmet_aspect() const;
    double helmet_edge(HelmetPiece piece) const;
    // The visor is drawn as a tint (glass): asked for (visorBlend 1), or automatic (0) for an opaque image.
    bool visor_tints(int visorBlend) const;
    double visor_white_point() const { return info_[0].whitePoint; }
    // The images' rows the pieces draw on (kHelmetRows bins, helmet.h's helmet_bands), with the visor slid up
    // `visorOffset` image heights; a glass visor (visorBlend as EyeComposition's) only where it tints.
    std::vector<bool> helmet_rows(double visorOffset, int visorBlend) const;
    // The helmet's light estimate (linear RGB per eye, per side: left, right, top, bottom of the eye's view),
    // for the log. request_light copies it at the next light pass, without stalling; read_light returns that
    // copy once the GPU has made it (false until then, or none asked).
    virtual void request_light() = 0;
    virtual bool read_light(float (&rgb)[2][4][3]) = 0;

protected:
    // The compositor's side: whether it can take a piece now, and the piece's texture made from `pixels`
    // (premultiplied, sRGB-encoded RGBA8) or dropped.
    virtual bool pieces_ready() const = 0;
    virtual bool upload_piece(int piece, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) = 0;
    virtual void drop_piece(int piece) = 0;
    void clear_pieces();

private:
    HelmetPieceInfo info_[3];  // HelmetPiece order
    bool loaded_[3] = {};
};

}  // namespace simqvfr
