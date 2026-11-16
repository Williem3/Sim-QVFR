// The pass trace of LMU's multi-view path (mvr_pass.h mvr_pass_trace; diagnostics, log only): mvr_pass_internal.h.
#include "mvr_pass_internal.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "abi.h"
#include "yebis_shaders.h"

namespace lmu::mvr_detail {
namespace {

constexpr int kPassTraceAt[] = {2, 3, 45};
// The frames of kPassTraceAt traced: every one with debug logs, else the last alone (the passes settled at QVFR's sizes).
int pt_first() { return simqvfr::log_debug() ? 0 : static_cast<int>(std::size(kPassTraceAt)) - 1; }
constexpr std::size_t kPassTraceEvents = 400;
constexpr std::size_t kPassTraceShapes = 40;

// The game's graphics settings, read from its own files (not its memory): one line.
std::string pt_game_settings() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n == MAX_PATH)
        return "?";
    const std::filesystem::path user = std::filesystem::path(exe).parent_path() / L"UserData";
    std::string line;
    const std::wstring config = (user / L"Config_DX11_VR.ini").wstring();
    for (const wchar_t* key : {L"EPostProcessingSettings", L"MSAA", L"UseFXAA", L"UpscaleMethod", L"RenderScale", L"ShaderLevel"}) {
        wchar_t value[64] = {};
        GetPrivateProfileStringW(L"COMPONENTS", key, L"?", value, 64, config.c_str());
        char text[160];
        std::snprintf(text, sizeof(text), "%s%ls=%ls", line.empty() ? "" : " ", key, value);
        line += text;
    }
    std::string json;
    if (HANDLE f = CreateFileW((user / L"player" / L"Settings.JSON").wstring().c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 22)) {
            json.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD got = 0;
            if (!ReadFile(f, json.data(), static_cast<DWORD>(json.size()), &got, nullptr))
                got = 0;
            json.resize(got);
        }
        CloseHandle(f);
    }
    line += ";";
    for (const char* key : {"Environment Reflections", "Road Reflections", "Shadows", "Shadow Blur", "Soft Particles", "Motion blur",
                            "Rain FX Quality", "Raindrop Flow", "Rainspray Flow", "Opponent Detail", "Player Detail", "Texture Detail",
                            "Special FX", "Circuit Detail", "Sun Occlusion", "Rearview Driving", "Max Visible Vehicles",
                            "Heat FX Fade Speed", "Live TV Displays"}) {
        const std::string quoted = std::string("\"") + key + "\":";
        const std::size_t at = json.find(quoted);
        if (at == std::string::npos)
            continue;
        std::size_t from = at + quoted.size(), to = json.find_first_of(",\r\n}", from);
        while (from < json.size() && json[from] == ' ')
            ++from;
        line += " " + std::string(key) + " " + json.substr(from, to == std::string::npos ? 8 : to - from) + ",";
    }
    // The renderer's own state (a read of the game's memory, this diagnostic's alone): the post-processing bits and the
    // flags a static reading of its post-processing function names. Their offsets are from that reading, not verified.
    const std::uintptr_t base = simqvfr::module_base();
    if (const auto* renderer = *reinterpret_cast<const std::uint8_t* const*>(base + kRenderer)) {
        std::uint32_t post = 0, msaa = 0;
        std::memcpy(&post, renderer + 0x7770c, sizeof(post));
        std::memcpy(&msaa, renderer + 0x1a1c, sizeof(msaa));
        char text[160];
        std::snprintf(text, sizeof(text), " renderer: post bits 0x%08x (bits 24-26: %u%u%u), +0x770fa %u, +0x1a1c %u", post,
                      (post >> 24) & 1, (post >> 25) & 1, (post >> 26) & 1, static_cast<unsigned>(renderer[0x770fa]), msaa);
        line += text;
    }
    return line;
}

}  // namespace

void pt_event(const char* format, ...) {
    if (pt.events.size() >= kPassTraceEvents) {
        ++pt.dropped;
        return;
    }
    char head[32];
    std::snprintf(head, sizeof(head), "@%llu ", static_cast<unsigned long long>(tr.draws));
    char body[320];
    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    pt.events.push_back(std::string(head) + body);
}

// A texture as "#n", with its size, format and sample count at its first mention this frame; "-" for none.
std::string pt_tex(ID3D11Resource* resource) {
    if (!resource)
        return "-";
    for (std::size_t i = 0; i < pt.ids.size(); ++i)
        if (pt.ids[i] == resource)
            return "#" + std::to_string(i);
    pt.ids.push_back(resource);
    std::string s = "#" + std::to_string(pt.ids.size() - 1);
    ComPtr<ID3D11Texture2D> t;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&t)))) {
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        char desc[64];
        std::snprintf(desc, sizeof(desc), "(%ux%u f%d x%u%s)", d.Width, d.Height, static_cast<int>(d.Format),
                      d.SampleDesc.Count, d.ArraySize > 1 ? " array" : "");
        s += desc;
    } else {
        s += "(not 2D)";
    }
    return s;
}

namespace {

std::string pt_view(ID3D11View* view) {
    if (!view)
        return "-";
    ComPtr<ID3D11Resource> r;
    view->GetResource(&r);
    return pt_tex(r.Get());
}

}  // namespace

// A draw on the game's context with one of YEBIS's pixel shaders (copy, composite, final blit): its input and target.
void pt_draw(ID3D11DeviceContext* context, const char* call) {
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    const UINT role = role_of_ps(ps.Get());
    if (!role)
        return;
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    const char* name = role == static_cast<UINT>(YebisRole::copy)        ? "copy"
                       : role == static_cast<UINT>(YebisRole::composite) ? "composite"
                       : role == static_cast<UINT>(YebisRole::blit)      ? "final blit"
                                                                         : "?";
    const std::string in = pt_view(srv.Get()), out = pt_view(rtv.Get());
    pt_event("YEBIS %s (%s) t0 %s > %s, %s", name, call, in.c_str(), out.c_str(),
             tr.open ? ("in tracked pass " + std::to_string(tr.pass)).c_str() : "outside the tracked passes");
}

namespace {

// A frame's events written out, a few to a line.
void pt_log_events() {
    std::string line;
    for (const std::string& e : pt.events) {
        if (!line.empty() && line.size() + e.size() > 1300) {
            simqvfr::logf("lmu mvr: pass trace: %s", line.c_str());
            line.clear();
        }
        line += line.empty() ? e : "; " + e;
    }
    if (!line.empty())
        simqvfr::logf("lmu mvr: pass trace: %s", line.c_str());
}

// The frame's shape (PassTrace): per tracked pass, what it began on, what is bound between its resolve and YEBIS's first
// draw (a stage there is what the first shape rule missed: it took YEBIS's metering copies, which come and go, and every
// bind after the resolve, for shape), whether YEBIS's first copy reads the resolve, whether the composite and the final
// blit were seen, and why it ended. A frame without tracked passes: whether YEBIS drew at all. `yebis`: it did.
std::string pt_shape(bool& yebis) {
    const auto id_after = [](const std::string& k, const char* mark) -> std::string {
        const std::size_t at = k.find(mark);
        if (at == std::string::npos)
            return {};
        std::size_t i = at + std::strlen(mark), j = i;
        while (j < k.size() && k[j] >= '0' && k[j] <= '9')
            ++j;
        return k.substr(i, j - i);
    };
    const auto size_of = [](const std::string& k, std::size_t from) -> std::string {  // "(WxH fN xS)" right after an id
        const std::size_t open = k.find('(', from), close = open == std::string::npos ? open : k.find(')', open);
        return close == std::string::npos ? std::string() : k.substr(open, close - open + 1);
    };
    std::string shape, resolveTo;
    bool inPass = false, resolved = false, copySeen = false, composite = false, blit = false;
    yebis = false;
    int passes = 0;
    for (const std::string& e : pt.events) {
        const std::size_t space = e.find(' ');
        const std::string k = space == std::string::npos ? e : e.substr(space + 1);
        const auto starts = [&k](const char* with) { return k.rfind(with, 0) == 0; };
        if (starts("YEBIS"))
            yebis = true;
        if (starts("P+")) {
            inPass = true;
            resolved = copySeen = composite = blit = false;
            resolveTo.clear();
            ++passes;
            shape += k.substr(0, k.find(';')) + " [";
        } else if (starts("P-") && inPass) {
            shape += std::string(copySeen ? "" : " NO-COPY") + (composite ? " composite" : " NO-COMPOSITE") +
                     (blit ? " blit" : " NO-BLIT") + "] " + k.substr(3) + " | ";
            inPass = false;
        } else if (!inPass) {
            continue;
        } else if (starts("R ")) {
            resolved = true;
            resolveTo = id_after(k, "> #");
            shape += " R";
        } else if (starts("B ") && resolved && !copySeen) {  // between the resolve and YEBIS: a stage
            const std::string target = id_after(k, "B #");
            const std::size_t depthAt = k.find(" depth ");
            shape += " B(" + (target.empty() ? std::string("-") : target == resolveTo ? std::string("the resolve") : "t" + size_of(k, 2)) +
                     (depthAt != std::string::npos && k.compare(depthAt + 7, 1, "-") != 0 ? " +depth)" : ")");
        } else if (starts("YEBIS copy") && resolved && !copySeen) {
            copySeen = true;
            const std::string from = id_after(k, "t0 #");
            shape += from == resolveTo ? " copy<-resolve" : " copy<-OTHER" + size_of(k, k.find("t0 #"));
        } else if (starts("YEBIS composite") && resolved) {
            composite = true;
        } else if (starts("YEBIS final blit") && resolved && copySeen) {
            blit = true;
        }
    }
    if (inPass)
        shape += " (open at the frame's end)";
    if (!passes)
        shape = yebis ? "no tracked pass, YEBIS drew" : "no tracked pass";
    return shape;
}

}  // namespace

// The frame end (after the tracker's): the trace logged if this frame is one of kPassTraceAt, or with lmu_pass_trace
// if its shape is new; the next frame's decided.
void pt_frame_end(unsigned long long frame, int passes) {
    if (passes > 0)
        ++pt.framesWithPasses;
    const char* const key = "@N = the game's draws this frame so far, #n = a texture (size, format, samples at its first "
                            "mention); B = a render target bind (target 0, depth), R = a resolve, C = a "
                            "CopySubresourceRegion, P+/P- = a tracked pass's begin/end, W = the focus post's window opened";
    if (pt.level && pt.recording) {
        // The game's settings, when its files change (looked at every two seconds or so).
        if (frame - pt.settingsCheckedFrame >= 180 || pt.settings.empty()) {
            pt.settingsCheckedFrame = frame;
            std::string now = pt_game_settings();
            if (now != pt.settings) {
                simqvfr::logf("lmu mvr: pass trace: frame %llu: the game's settings%s: %s", frame, pt.settings.empty() ? "" : " CHANGED",
                              now.c_str());
                pt.settings = std::move(now);
            }
        }
        bool yebis = false;
        const std::string shape = pt_shape(yebis);
        if (passes > 0 || yebis) {
            uint64_t hash = 1469598103934665603ull;
            for (const char c : shape)
                hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
            int index = -1;
            for (std::size_t i = 0; i < pt.shapes.size(); ++i)
                if (pt.shapes[i] == hash)
                    index = static_cast<int>(i);
            if (index < 0 && pt.shapes.size() < kPassTraceShapes) {
                pt.shapes.push_back(hash);
                index = static_cast<int>(pt.shapes.size()) - 1;
                simqvfr::logf("lmu mvr: pass trace: frame %llu: SHAPE %d, new (%d tracked passes): %zu events, %d not logged (over "
                              "%zu); %s",
                              frame, index, passes, pt.events.size(), pt.dropped, kPassTraceEvents, key);
                simqvfr::logf("lmu mvr: pass trace: shape %d with the game's settings: %s", index, pt_game_settings().c_str());
                pt_log_events();
                simqvfr::logf("lmu mvr: pass trace: shape %d is: %s", index, shape.c_str());
                pt.lastShape = index;
                pt.lastSwitchFrame = frame;
            } else if (index >= 0 && index != pt.lastShape && frame - pt.lastSwitchFrame >= 45) {
                simqvfr::logf("lmu mvr: pass trace: frame %llu: shape %d again (was %d; %d tracked passes)", frame, index, pt.lastShape,
                              passes);
                pt.lastShape = index;
                pt.lastSwitchFrame = frame;
            }
        }
    } else if (pt.recording && passes > 0 && pt_first() + pt.logged < static_cast<int>(std::size(kPassTraceAt)) &&
               pt.framesWithPasses == kPassTraceAt[pt_first() + pt.logged]) {
        ++pt.logged;
        simqvfr::logf("lmu mvr: pass trace: frame %llu (number %d with tracked passes, %d of them): %zu events, %d not "
                      "logged (over %zu); %s",
                      frame, pt.framesWithPasses, passes, pt.events.size(), pt.dropped, kPassTraceEvents, key);
        pt_log_events();
    }
    pt.events.clear();
    pt.ids.clear();
    pt.dropped = 0;
    pt.recording = tr.on && tr.driveBegin &&
                   (pt.level ? true
                             : pt_first() + pt.logged < static_cast<int>(std::size(kPassTraceAt)) &&
                                   pt.framesWithPasses + 1 == kPassTraceAt[pt_first() + pt.logged]);
}

}  // namespace lmu::mvr_detail

namespace lmu {

using namespace mvr_detail;

void mvr_pass_trace(int level) {
    pt.level = level;
    pt.shapes.clear();
    pt.lastShape = -1;
    pt.settings.clear();
    if (level)
        simqvfr::log_line("lmu mvr: pass trace on (lmu_pass_trace): every frame recorded, a frame logged when its shape is new, "
                          "with the game's settings as its files hold them");
}

}  // namespace lmu
