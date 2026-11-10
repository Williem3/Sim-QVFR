// Offline check of LMU's MVR probe (games/lmu/mvr_probe.h) on a WARP device: installed at instance creation it hooks
// device creation, then the device's vertex shader creation; the game's shaders still come back as created, each is
// converted (or refused, and why) and counted. NVAPI isn't asked on WARP (not an NVIDIA device).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "engine/nvapi_gpus.h"
#include "games/ace/game.h"
#include "games/lmu/game.h"
#include "games/lmu/mvr_probe.h"
#include "games/multi_view.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

std::vector<uint8_t> compile(const std::string& source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "test", nullptr, nullptr, "main", profile, 0, 0, &code, &errors)))
        return {};
    const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    return {p, p + code->GetBufferSize()};
}

// The position and `n` texcoords: 15 outputs leave no two registers for the second view.
std::string outputs(int n) {
    std::string s = "struct Out { float4 pos : SV_Position;";
    for (int i = 0; i < n; ++i)
        s += " float4 t" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";";
    s += " }; Out main(float4 p : POSITION) { Out o; o.pos = p;";
    for (int i = 0; i < n; ++i)
        s += " o.t" + std::to_string(i) + " = p * " + std::to_string(i + 2) + ";";
    return s + " return o; }";
}

}  // namespace

int main() {
    // d3d11.dll's code before the probe: device creation (hooked until mvr_probe_device) and a device's shader creation
    // (never: an entry of a vtable copy the game's device points at).
    const auto* exportCode = reinterpret_cast<const uint8_t*>(GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDevice"));
    uint8_t exportBefore[16] = {}, shaderCodeBefore[16] = {};
    const uint8_t* shaderCode = nullptr;
    void** ownVtable = nullptr;
    {
        ComPtr<ID3D11Device> before;
        if (exportCode && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                                      D3D11_SDK_VERSION, &before, nullptr, nullptr))) {
            ownVtable = *reinterpret_cast<void***>(before.Get());
            shaderCode = static_cast<const uint8_t*>(ownVtable[12]);
            std::memcpy(shaderCodeBefore, shaderCode, sizeof(shaderCodeBefore));
            std::memcpy(exportBefore, exportCode, sizeof(exportBefore));
        }
    }
    check(shaderCode != nullptr, "d3d11.dll's code read before the probe");
    check(lmu::mvr_probe_install(nullptr, true), "installed at instance creation (twins attached, as for multi-view)");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("FAIL no WARP device\n");
        return 1;
    }
    check(shaderCode && *reinterpret_cast<void***>(device.Get()) != ownVtable &&
              std::memcmp(shaderCodeBefore, shaderCode, sizeof(shaderCodeBefore)) == 0,
          "the device on a vtable copy: d3d11.dll's CreateVertexShader code as it was");
    const std::vector<uint8_t> plain = compile(outputs(2), "vs_4_0");
    const std::vector<uint8_t> full = compile(outputs(14), "vs_4_0");
    const std::vector<uint8_t> pixel = compile("float4 main() : SV_Target { return 1; }", "ps_4_0");
    ComPtr<ID3D11VertexShader> a, b;
    ComPtr<ID3D11PixelShader> ps;
    const bool created = SUCCEEDED(device->CreateVertexShader(plain.data(), plain.size(), nullptr, &a)) && a &&
                         SUCCEEDED(device->CreateVertexShader(full.data(), full.size(), nullptr, &b)) && b &&
                         SUCCEEDED(device->CreatePixelShader(pixel.data(), pixel.size(), nullptr, &ps)) && ps;
    check(created, "the game's shaders are created as before");
    const lmu::MvrProbeStats s = lmu::mvr_probe_stats();
    char what[200];
    std::snprintf(what, sizeof(what), "counted: %llu vertex shaders, %llu converted, %llu refused for no free register",
                  static_cast<unsigned long long>(s.created), static_cast<unsigned long long>(s.converted),
                  static_cast<unsigned long long>(s.refused.count("no free output register")
                                                      ? s.refused.at("no free output register") : 0));
    check(s.created == 2 && s.converted == 1 && s.refused.size() == 1 && s.refused.count("no free output register") &&
              s.refused.at("no free output register") == 1,
          what);
    check(s.nvapiCreated == 0 && s.nvapiFailed == 0, "NVAPI not asked on WARP");
    // Instancing on WARP (a vertex shader may pick the viewport there): both shaders get an instanced twin (one free
    // register is enough), attached under its own GUID; no single-pass stereo twin.
    {
        const auto twin = [](ID3D11VertexShader* vs, const GUID& guid) {
            IUnknown* u = nullptr;
            UINT size = sizeof(u);
            const bool has = SUCCEEDED(vs->GetPrivateData(guid, &size, &u)) && u;
            if (u)
                u->Release();
            return has;
        };
        std::snprintf(what, sizeof(what), "instancing: %llu instanced twins converted, %llu created, %llu failed; attached "
                      "to both, no single-pass stereo twin; the backend is instancing",
                      static_cast<unsigned long long>(s.instancedConverted),
                      static_cast<unsigned long long>(s.instancedCreated), static_cast<unsigned long long>(s.instancedFailed));
        check(s.instancedConverted == 2 && s.instancedCreated == 2 && s.instancedFailed == 0 && created &&
                  twin(a.Get(), lmu::kMvrInstancedTwinGuid) && twin(b.Get(), lmu::kMvrInstancedTwinGuid) &&
                  !twin(a.Get(), lmu::kMvrTwinGuid) && lmu::mvr_probe_usable() &&
                  lmu::mvr_probe_backend() == lmu::MvrBackend::instanced && lmu::mvr_probe_instanced(),
              what);
        // Input layouts: one with per-instance data gets a twin with its step rate doubled; a per-vertex one doesn't.
        const std::vector<uint8_t> perInstance =
            compile("float4 main(float4 p : POSITION, float4 o : OFFSET) : SV_Position { return p + o; }", "vs_4_0");
        const D3D11_INPUT_ELEMENT_DESC el[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"OFFSET", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 3}};
        const std::vector<uint8_t> perVertex = compile("float4 main(float4 p : POSITION) : SV_Position { return p; }", "vs_4_0");
        ComPtr<ID3D11InputLayout> vertexOnly, instanceData;
        device->CreateInputLayout(el, 1, perVertex.data(), perVertex.size(), &vertexOnly);
        device->CreateInputLayout(el, 2, perInstance.data(), perInstance.size(), &instanceData);
        const auto layoutTwin = [](ID3D11InputLayout* l) {
            IUnknown* u = nullptr;
            UINT size = sizeof(u);
            const bool has = l && SUCCEEDED(l->GetPrivateData(lmu::kMvrInstancedLayoutGuid, &size, &u)) && u;
            if (u)
                u->Release();
            return has;
        };
        const lmu::MvrProbeStats ls = lmu::mvr_probe_stats();
        check(vertexOnly && instanceData && !layoutTwin(vertexOnly.Get()) && layoutTwin(instanceData.Get()) &&
                  ls.layouts == 2 && ls.instancedLayouts == 1 && lmu::mvr_probe_instanced_layouts() == 1,
              "instancing: the layout with per-instance data twinned (step rate doubled), the per-vertex one not");
    }
    lmu::mvr_probe_frame_end();  // the summary (log only)
    // xrCreateSession: device creation unhooked, its code as it was; the device's shader creation still seen.
    check(exportCode && std::memcmp(exportBefore, exportCode, sizeof(exportBefore)) != 0,
          "device creation hooked before the session");
    lmu::mvr_probe_device(device.Get());
    check(exportCode && std::memcmp(exportBefore, exportCode, sizeof(exportBefore)) == 0,
          "device creation unhooked at the session: D3D11CreateDevice's code as it was");
    ComPtr<ID3D11VertexShader> c;
    check(SUCCEEDED(device->CreateVertexShader(plain.data(), plain.size(), nullptr, &c)) && c &&
              lmu::mvr_probe_stats().created == 3,
          "the device's shader creation still seen after the session starts");

    // Which GPUs draw two full views per draw: Turing (RTX 20) and newer, not Tegra's ids.
    namespace nvapi = simqvfr::nvapi;
    check(!nvapi::has_two_views(0x140) && !nvapi::has_two_views(0x150) && nvapi::has_two_views(0x160) &&
              nvapi::has_two_views(0x170) && nvapi::has_two_views(0x190) && nvapi::has_two_views(0x1B0) &&
              !nvapi::has_two_views(0xE0000040u) && !nvapi::has_two_views(0),
          "two views from Turing (0x160) on: Volta, Tegra and unknown not");
    check(nvapi::offers_multi_view(0x160, 2, "NVIDIA GeForce RTX 2080 Ti") &&
              nvapi::offers_multi_view(0x1B0, 3, "NVIDIA GeForce RTX 5070 Ti") &&
              !nvapi::offers_multi_view(0x160, 8, "NVIDIA GeForce GTX 1660 SUPER") &&
              !nvapi::offers_multi_view(0x160, 7, "NVIDIA GeForce GTX 1650") &&
              !nvapi::offers_multi_view(0x160, 6, "NVIDIA GeForce GTX 1650") &&
              !nvapi::offers_multi_view(0x150, 0, "NVIDIA TITAN V") && !nvapi::offers_multi_view(0x130, 4, "GTX 1080"),
          "multi-view offered on RTX 20 and newer only: not the GTX 16 series (TU116, TU117, or named GTX), not Pascal or Volta");
    // The layer's decision (games/multi_view.h) from an ini: multi_view=1 for a game that has it, on such a GPU.
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring ini = std::wstring(temp) + L"simqvfr-multiview-" + std::to_wstring(GetCurrentProcessId()) + L".ini";
        WritePrivateProfileStringW(L"experiment", L"cpu_profile", L"0", ini.c_str());  // an ini without multi_view
        std::string why;
        check(simqvfr::games::multi_view_on(&lmu::kGame, ini.c_str(), &why), "no multi_view key: on (the default)");
        WritePrivateProfileStringW(L"experiment", L"multi_view", L"0", ini.c_str());
        check(!simqvfr::games::multi_view_on(&lmu::kGame, ini.c_str(), &why) && why == "off (multi_view=0)",
              "multi_view=0: off");
        WritePrivateProfileStringW(L"experiment", L"multi_view", L"1", ini.c_str());
        check(!simqvfr::games::multi_view_on(&ace::kGame, ini.c_str(), &why) && why == "off: not for this game",
              "multi_view=1 for a game without it (ACE): off");
        const bool on = simqvfr::games::multi_view_on(&lmu::kGame, ini.c_str(), &why);
        std::snprintf(what, sizeof(what), "multi_view=1 for LMU: on whatever the GPU (%s)", why.c_str());
        check(on && !why.empty() &&
                  (why.find("Sim-QVFR's own") == std::string::npos) == (nvapi::two_views_gpu() != nullptr),
              what);
        WritePrivateProfileStringW(L"experiment", L"multi_view_software", L"1", ini.c_str());
        const bool sw = simqvfr::games::multi_view_on(&lmu::kGame, ini.c_str(), &why);
        std::snprintf(what, sizeof(what), "multi_view_software=1: instancing (%s)", why.c_str());
        check(sw && why.find("instancing") != std::string::npos, what);
        DeleteFileW(ini.c_str());
    }
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
