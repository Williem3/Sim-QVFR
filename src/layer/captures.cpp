// The debug captures of the composition (capture_every_s) and the draw census hotkey's (development builds): PNGs in
// the log folder's captures.
#include <filesystem>
#include <mutex>

#include "log.h"
#include "quad/compositor.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

constexpr int kMaxCaptures = 8;

}  // namespace

bool quad_capture_due() {
    if (!experiment.captureEverySeconds || quad.captures >= kMaxCaptures)
        return false;
    const ULONGLONG now = GetTickCount64();
    if (quad.lastCapture == 0) {  // first capture one interval after the pipeline starts
        quad.lastCapture = now;
        return false;
    }
    return now - quad.lastCapture >= experiment.captureEverySeconds * 1000ull;
}

// Called with quad.mutex held, after compose and before the output image is released.
void views4_capture(ID3D12Resource* views, ID3D12Resource* output) {
    quad.lastCapture = GetTickCount64();
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"v4cap%02d-%02u%02u%02u", ++quad.captures, t.wHour, t.wMinute, t.wSecond);
    const bool ok = quad.compositor.save_views4_captures(views, output, (dir / name).wstring());
    logf("views4: capture %ls %s (all four views, and the composed left eye)", name, ok ? "saved" : "FAILED");
}

void views4_census_capture(ID3D12Resource* views, ID3D12Resource* focus, D3D12_RESOURCE_STATES focusState,
                           ID3D12Resource* output, int index) {
    quad.lastCapture = GetTickCount64();
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    static wchar_t stem[64];
    if (index == 0) {  // one name per burst
        SYSTEMTIME t;
        GetLocalTime(&t);
        swprintf_s(stem, L"v4cens%02d-%02u%02u%02u", ++quad.captures, t.wHour, t.wMinute, t.wSecond);
    }
    wchar_t name[96];
    swprintf_s(name, L"%ls-f%d", stem, index);
    const bool ok = quad.compositor.save_census_captures(views, focus, focusState, output, (dir / name).wstring());
    logf("replay census: capture %ls %s (ACE's eyes, the focus views, the output; both eyes)", name, ok ? "saved" : "FAILED");
}

}  // namespace simqvfr::layer
