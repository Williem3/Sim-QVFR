#include "pix_capture.h"

#include <filesystem>
#include <string>
#include <system_error>

#include "../log.h"
#include "../xr.h"

namespace aceqv {
namespace {

// pix3_win.h: PIXCaptureParameters' GPU member starts with the output file name.
struct GpuCaptureParameters {
    const wchar_t* fileName;
};
using BeginFn = HRESULT(WINAPI*)(const GpuCaptureParameters*);
using EndFn = HRESULT(WINAPI*)();

BeginFn beginCapture = nullptr;
EndFn endCapture = nullptr;
uint64_t captureFrame = 0;
std::wstring captureFile;
bool capturing = false;

// The newest "Microsoft PIX\<version>" folder holding WinPixGpuCapturer.dll.
std::filesystem::path newest_capturer() {
    std::error_code ec;
    const std::filesystem::path root = L"C:\\Program Files\\Microsoft PIX";
    std::filesystem::path best;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        const auto dll = entry.path() / L"WinPixGpuCapturer.dll";
        if (entry.is_directory(ec) && std::filesystem::exists(dll, ec) && (best.empty() || entry.path().filename() > best.parent_path().filename()))
            best = dll;
    }
    return best;
}

}  // namespace

bool pix_capture_load(uint64_t frame) {
    const std::filesystem::path dll = newest_capturer();
    HMODULE module = dll.empty() ? nullptr : LoadLibraryW(dll.c_str());
    if (module) {
        beginCapture = reinterpret_cast<BeginFn>(GetProcAddress(module, "BeginProgrammaticGpuCapture"));
        endCapture = reinterpret_cast<EndFn>(GetProcAddress(module, "EndProgrammaticGpuCapture"));
    }
    if (!beginCapture || !endCapture) {
        logf("pix: GPU capturer not available (%ls)", dll.empty() ? L"PIX not installed" : dll.c_str());
        return false;
    }
    std::error_code ec;
    const auto dir = std::filesystem::path(log_directory()) / L"pix";
    std::filesystem::create_directories(dir, ec);
    captureFile = (dir / (L"ace-frame-" + std::to_wstring(frame) + L".wpix")).wstring();
    captureFrame = frame;
    logf("pix: capturer loaded from %ls; capturing from xrEndFrame %llu to %llu into %ls", dll.c_str(),
         static_cast<unsigned long long>(frame), static_cast<unsigned long long>(frame + 2), captureFile.c_str());
    return true;
}

void pix_capture_frame(uint64_t endFrame) {
    if (!beginCapture || captureFrame == 0)
        return;
    if (endFrame == captureFrame && !capturing) {
        const GpuCaptureParameters parameters{captureFile.c_str()};
        const HRESULT hr = beginCapture(&parameters);
        capturing = SUCCEEDED(hr);
        logf("pix: begin capture at xrEndFrame %llu: hr=0x%08lx", static_cast<unsigned long long>(endFrame),
             static_cast<unsigned long>(hr));
    } else if (endFrame == captureFrame + 2 && capturing) {
        const HRESULT hr = endCapture();
        capturing = false;
        logf("pix: end capture at xrEndFrame %llu: hr=0x%08lx (file %ls)", static_cast<unsigned long long>(endFrame),
             static_cast<unsigned long>(hr), captureFile.c_str());
    }
}

}  // namespace aceqv
