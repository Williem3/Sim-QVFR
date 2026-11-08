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

#include "games/lmu/mvr_probe.h"

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
    check(lmu::mvr_probe_install(nullptr), "installed at instance creation");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("FAIL no WARP device\n");
        return 1;
    }
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
    lmu::mvr_probe_frame_end();  // the summary (log only)
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
