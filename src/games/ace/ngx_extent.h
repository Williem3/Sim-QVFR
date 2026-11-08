// Each DLSS feature's created output size, put back into NGX's parameter bag before the feature is evaluated
// (dlss_extent.cpp). ACE makes its per-view features through one bag and never sets OutWidth/OutHeight when it
// evaluates them, so with views of two sizes (views4) the bag holds the last-made view's size at every evaluation.
// Pure: the bag is NGX's NVSDK_NGX_Parameter, reached through its vtable (no NGX headers); tests/ace_ngx_extent_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace simqvfr {

// NVSDK_NGX_Parameter (nvsdk_ngx_defs.h) as MSVC lays out its vtable: the eight Set overloads, then the eight Get
// overloads, each group in reverse declaration order. Set(name, unsigned int) and Get(name, unsigned int*): ACE's own
// NVSDK_NGX_Parameter_SetUI ("OutWidth" at create) and GetUI ("SuperSampling.MinDriverVersionMajor") use these.
inline constexpr std::ptrdiff_t kNgxSetUiOffset = 0x20, kNgxGetUiOffset = 0x60;
inline constexpr unsigned kNgxFeatureSuperSampling = 1;  // NVSDK_NGX_Feature_SuperSampling
inline bool ngx_succeeded(unsigned result) { return (result & 0xfff00000u) != 0xbad00000u; }  // NVSDK_NGX_SUCCEED

using NgxSetUiFn = void (*)(void* bag, const char* name, unsigned value);
using NgxGetUiFn = unsigned (*)(void* bag, const char* name, unsigned* value);

inline void ngx_set_ui(void* bag, const char* name, unsigned value) {
    const auto* vtable = *static_cast<const std::uint8_t* const*>(bag);
    (*reinterpret_cast<const NgxSetUiFn*>(vtable + kNgxSetUiOffset))(bag, name, value);
}
// False when the bag has no such value (value then unchanged).
inline bool ngx_get_ui(void* bag, const char* name, unsigned& value) {
    const auto* vtable = *static_cast<const std::uint8_t* const*>(bag);
    return ngx_succeeded((*reinterpret_cast<const NgxGetUiFn*>(vtable + kNgxGetUiOffset))(bag, name, &value));
}

// The output size of each SuperSampling feature, by its handle (NVSDK_NGX_Handle*). ACE makes one feature per view
// and again with every new renderer (a graphics settings change), and NGX may hand a released handle out again: the
// newest record of a handle wins, the oldest of 32 makes room.
class NgxExtents {
public:
    void record(const void* handle, unsigned width, unsigned height) {
        for (std::size_t i = 0; i < count_; ++i)
            if (entries_[i].handle == handle) {
                entries_[i].width = width;
                entries_[i].height = height;
                return;
            }
        if (count_ < kCapacity) {
            entries_[count_++] = {handle, width, height};
            return;
        }
        entries_[next_] = {handle, width, height};  // the oldest record
        next_ = (next_ + 1) % kCapacity;
    }
    bool find(const void* handle, unsigned& width, unsigned& height) const {
        for (std::size_t i = 0; i < count_; ++i)
            if (entries_[i].handle == handle) {
                width = entries_[i].width;
                height = entries_[i].height;
                return true;
            }
        return false;
    }

private:
    static constexpr std::size_t kCapacity = 32;
    struct Entry {
        const void* handle;
        unsigned width, height;
    };
    Entry entries_[kCapacity] = {};
    std::size_t count_ = 0, next_ = 0;
};

// Before an evaluation: the feature's output size into the bag's OutWidth/OutHeight. True when the bag held another
// size (it is then corrected); false when it already held this one.
inline bool ngx_put_output_extent(void* bag, unsigned width, unsigned height) {
    unsigned w = 0, h = 0;
    const bool held = ngx_get_ui(bag, "OutWidth", w) && ngx_get_ui(bag, "OutHeight", h);
    if (held && w == width && h == height)
        return false;
    ngx_set_ui(bag, "OutWidth", width);
    ngx_set_ui(bag, "OutHeight", height);
    return true;
}

}  // namespace simqvfr
