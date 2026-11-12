// Offline check of the car slot target for other cars' motion vectors (docs/recon-lmu.md "stage 3 design revised") on a
// WARP device (--hardware: the GPU's): how D3D11 treats an integer render target at slot 1 beside a blended HDR target at
// slot 0 (the game's), a pixel shader that doesn't write it, and one smaller than the game's target; then the pixel
// shader twin (engine/dxbc_patch.h add_car_slot) writing a draw's slot word, frame tag and depth.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "engine/dxbc_patch.h"

using Microsoft::WRL::ComPtr;
namespace dxbc = simqvfr::dxbc;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

ComPtr<ID3DBlob> compile(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "t", nullptr, nullptr, entry, target, 0, 0, &code, &errors))) {
        std::printf("FAIL compile %s: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

constexpr char kShaders[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0.25, 1);
}
struct Two {
    float4 colour : SV_Target0;
    uint word : SV_Target1;
};
Two both(float4 p : SV_Position) {
    Two o;
    o.colour = float4(1, 0.5, 0.25, 0.5);
    o.word = 0x12345678u;
    return o;
}
float4 one(float4 p : SV_Position) : SV_Target0 { return float4(0.25, 0.25, 0.25, 1); }
)";

// Like LMU's scene pixel shaders: a constant buffer, a discard, more than one return.
constexpr char kGamePs[] = R"(
cbuffer K : register(b0) { float4 tint; };
float4 game(float4 p : SV_Position) : SV_Target0 {
    if (p.x < 2)
        discard;
    if (p.y > 60)
        return tint * 0.5;
    return tint;
}
)";

// --scan <folder>: every PS_*.dxbc there (the game's shaders, saved by lmu_mvr_dump: local only) given the slot output,
// alone and after texture sharpening's mip bias twin, and created on the device; the reasons for any refusal.
int scan(ID3D11Device* device, const char* folder) {
    WIN32_FIND_DATAA f;
    const std::string pattern = std::string(folder) + "\\PS_*.dxbc";
    HANDLE h = FindFirstFileA(pattern.c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("FAIL no PS_*.dxbc in %s\n", folder);
        return 1;
    }
    int files = 0, made = 0, created = 0, biased = 0, biasedMade = 0, biasedCreated = 0;
    std::vector<std::string> refused;
    do {
        std::FILE* file = nullptr;
        if (fopen_s(&file, (std::string(folder) + "\\" + f.cFileName).c_str(), "rb") != 0 || !file)
            continue;
        std::vector<uint8_t> code;
        uint8_t buffer[65536];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
            code.insert(code.end(), buffer, buffer + n);
        std::fclose(file);
        ++files;
        const dxbc::SlotOutput s = dxbc::add_slot_output(code.data(), code.size());
        ComPtr<ID3D11PixelShader> ps;
        if (!s.bytecode.empty()) {
            ++made;
            created += SUCCEEDED(device->CreatePixelShader(s.bytecode.data(), s.bytecode.size(), nullptr, &ps)) ? 1 : 0;
        } else {
            refused.push_back(std::string(f.cFileName) + ": " + s.error);
        }
        const dxbc::FocusMipBias b = dxbc::add_focus_mip_bias(code.data(), code.size());
        if (!b.bytecode.empty()) {
            ++biased;
            const dxbc::SlotOutput sb = dxbc::add_slot_output(b.bytecode.data(), b.bytecode.size());
            ComPtr<ID3D11PixelShader> pb;
            if (!sb.bytecode.empty()) {
                ++biasedMade;
                biasedCreated +=
                    SUCCEEDED(device->CreatePixelShader(sb.bytecode.data(), sb.bytecode.size(), nullptr, &pb)) ? 1 : 0;
            }
        }
    } while (FindNextFileA(h, &f));
    FindClose(h);
    std::printf("%d pixel shaders: %d given the slot output, %d created; %d with a mip bias twin, %d of those given it too, "
                "%d created\n",
                files, made, created, biased, biasedMade, biasedCreated);
    for (const std::string& r : refused)
        std::printf("  refused %s\n", r.c_str());
    return made == files && created == made && biasedMade == biased && biasedCreated == biasedMade ? 0 : 1;
}

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

ComPtr<ID3D11Texture2D> texture(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT f, UINT bind) {
    D3D11_TEXTURE2D_DESC t = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    ComPtr<ID3D11Texture2D> tex;
    d->CreateTexture2D(&t, nullptr, &tex);
    return tex;
}

// Texel (x, y) of a 32-bit texture.
uint32_t read_u32(Device& g, ID3D11Texture2D* tex, UINT x, UINT y) {
    D3D11_TEXTURE2D_DESC t;
    tex->GetDesc(&t);
    t.Usage = D3D11_USAGE_STAGING;
    t.BindFlags = 0;
    t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    g.device->CreateTexture2D(&t, nullptr, &staging);
    g.context->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    uint32_t v = 0xdeadbeef;
    if (SUCCEEDED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        std::memcpy(&v, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4, 4);
        g.context->Unmap(staging.Get(), 0);
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    bool hardware = false;
    const char* scanFolder = nullptr;
    for (int i = 1; i < argc; ++i) {
        hardware = hardware || std::strcmp(argv[i], "--hardware") == 0;
        if (std::strcmp(argv[i], "--scan") == 0 && i + 1 < argc)
            scanFolder = argv[++i];
    }
    Device g;
    if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr,
                                 0, D3D11_SDK_VERSION, &g.device, nullptr, &g.context))) {
        std::printf("FAIL no %s device\n", hardware ? "hardware" : "WARP");
        return 1;
    }
    if (scanFolder)
        return scan(g.device.Get(), scanFolder);
    char what[240];
    const ComPtr<ID3DBlob> vsCode = compile(kShaders, "vs", "vs_5_0"), bothCode = compile(kShaders, "both", "ps_5_0"),
                           oneCode = compile(kShaders, "one", "ps_5_0");
    if (!vsCode || !bothCode || !oneCode)
        return 1;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> both, one;
    g.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    g.device->CreatePixelShader(bothCode->GetBufferPointer(), bothCode->GetBufferSize(), nullptr, &both);
    g.device->CreatePixelShader(oneCode->GetBufferPointer(), oneCode->GetBufferSize(), nullptr, &one);

    constexpr UINT W = 64, H = 64;
    const ComPtr<ID3D11Texture2D> hdr = texture(g.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET),
                                  slots = texture(g.device.Get(), W, H, DXGI_FORMAT_R32_UINT, D3D11_BIND_RENDER_TARGET),
                                  half = texture(g.device.Get(), W / 2, H / 2, DXGI_FORMAT_R32_UINT, D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> hdrRtv, slotsRtv, halfRtv;
    g.device->CreateRenderTargetView(hdr.Get(), nullptr, &hdrRtv);
    g.device->CreateRenderTargetView(slots.Get(), nullptr, &slotsRtv);
    g.device->CreateRenderTargetView(half.Get(), nullptr, &halfRtv);
    const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
    g.context->RSSetViewports(1, &vp);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->VSSetShader(vs.Get(), nullptr, 0);

    // Blend states as LMU's: target 0 blended with independent blend off (target 1 then takes target 0's), and with it on
    // and target 1 as D3D11's defaults would leave a desc zeroed (write mask 0) or as CD3D11 sets it (all, no blend).
    const auto blend = [&](bool independent, UINT8 mask1) {
        D3D11_BLEND_DESC b{};
        b.IndependentBlendEnable = independent;
        for (auto& rt : b.RenderTarget)
            rt = {FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO,
                  D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
        b.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                             D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
        b.RenderTarget[1].RenderTargetWriteMask = mask1;
        ComPtr<ID3D11BlendState> s;
        g.device->CreateBlendState(&b, &s);
        return s;
    };
    // A draw over the whole target into the HDR target and `second` (cleared to 7 first: an integer target's clear takes
    // the floats' values), with blend state `bs` and pixel shader `ps`.
    const auto draw_into = [&](ID3D11RenderTargetView* second, ID3D11PixelShader* ps, ID3D11BlendState* bs) {
        const float grey[4] = {0.5f, 0.5f, 0.5f, 1}, seven[4] = {7, 7, 7, 7};
        g.context->ClearRenderTargetView(hdrRtv.Get(), grey);
        g.context->ClearRenderTargetView(second, seven);
        ID3D11RenderTargetView* rtvs[2] = {hdrRtv.Get(), second};
        g.context->OMSetRenderTargets(2, rtvs, nullptr);
        g.context->OMSetBlendState(bs, nullptr, 0xffffffff);
        g.context->PSSetShader(ps, nullptr, 0);
        g.context->Draw(3, 0);
        g.context->OMSetRenderTargets(0, nullptr, nullptr);
        g.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    };
    std::printf("probes (%s; 0x12345678 = the word written, 7 = the target as cleared):\n", hardware ? "hardware" : "WARP");
    const ComPtr<ID3D11BlendState> shared = blend(false, D3D11_COLOR_WRITE_ENABLE_ALL),
                                   ownAll = blend(true, D3D11_COLOR_WRITE_ENABLE_ALL), ownNone = blend(true, 0);
    draw_into(slotsRtv.Get(), both.Get(), shared.Get());
    const uint32_t blendedShared = read_u32(g, slots.Get(), 5, 5);
    draw_into(slotsRtv.Get(), both.Get(), ownAll.Get());
    const uint32_t blendedOwn = read_u32(g, slots.Get(), 5, 5);
    draw_into(slotsRtv.Get(), both.Get(), ownNone.Get());
    const uint32_t maskedOff = read_u32(g, slots.Get(), 5, 5);
    draw_into(slotsRtv.Get(), one.Get(), nullptr);
    const uint32_t notWritten = read_u32(g, slots.Get(), 5, 5);
    draw_into(halfRtv.Get(), both.Get(), nullptr);
    const uint32_t halfInside = read_u32(g, half.Get(), 5, 5);
    const uint32_t hdrOutside = read_u32(g, hdr.Get(), 40, 40);  // R and G as halves: 1.0 = 0x3c00, 0.5 = 0x3800
    std::printf("  target 0 blended, independent blend off (target 1 takes its state): 0x%08x\n", blendedShared);
    std::printf("  independent blend on, target 1 unblended, all written:              0x%08x\n", blendedOwn);
    std::printf("  independent blend on, target 1's write mask 0:                      0x%08x\n", maskedOff);
    std::printf("  a pixel shader without SV_Target1:                                  0x%08x\n", notWritten);
    std::printf("  target 1 half the size: inside it 0x%08x; target 0 outside it R,G 0x%08x\n", halfInside, hdrOutside);
    check(blendedShared == 0x12345678u, "an integer target at slot 1 takes the word as written though target 0 blends");
    check(blendedOwn == 0x12345678u && maskedOff == 7u, "independent blend: target 1's own write mask decides");
    check(notWritten == 7u, "a pixel shader that doesn't write target 1 leaves it as it was");
    std::printf("  (a target 1 smaller than target 0: the draw drops out for both; the slot target is the scene's size)\n");

    // The twin: a pixel shader like LMU's (a constant buffer, a discard, two returns) given the slot output.
    const ComPtr<ID3DBlob> gameCode = compile(kGamePs, "game", "ps_5_0");
    if (!gameCode)
        return 1;
    const dxbc::SlotOutput twin =
        dxbc::add_slot_output(static_cast<const uint8_t*>(gameCode->GetBufferPointer()), gameCode->GetBufferSize());
    check(!twin.bytecode.empty(), twin.error.empty() ? "the twin made" : twin.error.c_str());
    ComPtr<ID3D11PixelShader> game, twinPs;
    g.device->CreatePixelShader(gameCode->GetBufferPointer(), gameCode->GetBufferSize(), nullptr, &game);
    const HRESULT created = twin.bytecode.empty() ? E_FAIL
                                                  : g.device->CreatePixelShader(twin.bytecode.data(), twin.bytecode.size(),
                                                                                nullptr, &twinPs);
    check(SUCCEEDED(created) && twinPs, "the device takes it");
    {
        ComPtr<ID3D11ShaderReflection> refl;
        D3D11_SHADER_DESC sd{};
        D3D11_SIGNATURE_PARAMETER_DESC out1{};
        const bool reflected = !twin.bytecode.empty() &&
                               SUCCEEDED(D3DReflect(twin.bytecode.data(), twin.bytecode.size(), IID_PPV_ARGS(&refl))) &&
                               SUCCEEDED(refl->GetDesc(&sd)) && sd.OutputParameters == 2 &&
                               SUCCEEDED(refl->GetOutputParameterDesc(1, &out1));
        check(reflected && std::strcmp(out1.SemanticName, "SV_Target") == 0 && out1.SemanticIndex == 1 &&
                  out1.Register == 1 && out1.ComponentType == D3D_REGISTER_COMPONENT_UINT32 && out1.Mask == 0x1,
              "its reflection: a second output, SV_Target1, a uint in register 1");
    }
    // The slot buffer: element i holds 0x1000 + i; the draw's view is element 3 alone.
    ComPtr<ID3D11Buffer> slotBuffer, tint;
    ComPtr<ID3D11ShaderResourceView> slotView;
    {
        uint32_t words[8];
        for (uint32_t i = 0; i < 8; ++i)
            words[i] = 0x1000 + i;
        const D3D11_BUFFER_DESC bd = {sizeof(words), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA init = {words, 0, 0};
        g.device->CreateBuffer(&bd, &init, &slotBuffer);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_UINT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.FirstElement = 3;
        sv.Buffer.NumElements = 1;
        g.device->CreateShaderResourceView(slotBuffer.Get(), &sv, &slotView);
        const float colour[4] = {0.75f, 0.5f, 0.25f, 1};
        const D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA ci = {colour, 0, 0};
        g.device->CreateBuffer(&cd, &ci, &tint);
    }
    const ComPtr<ID3D11Texture2D> slot16 = texture(g.device.Get(), W, H, DXGI_FORMAT_R16_UINT, D3D11_BIND_RENDER_TARGET),
                                  reference = texture(g.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                      D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> slot16Rtv, referenceRtv;
    g.device->CreateRenderTargetView(slot16.Get(), nullptr, &slot16Rtv);
    g.device->CreateRenderTargetView(reference.Get(), nullptr, &referenceRtv);
    g.context->PSSetConstantBuffers(0, 1, tint.GetAddressOf());
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1}, seven[4] = {7, 7, 7, 7};
    // The game's shader alone into the reference; the twin into the HDR target and the slot target.
    g.context->ClearRenderTargetView(referenceRtv.Get(), grey);
    g.context->OMSetRenderTargets(1, referenceRtv.GetAddressOf(), nullptr);
    g.context->PSSetShader(game.Get(), nullptr, 0);
    g.context->Draw(3, 0);
    const auto twin_draw = [&](ID3D11ShaderResourceView* view) {
        g.context->ClearRenderTargetView(hdrRtv.Get(), grey);
        g.context->ClearRenderTargetView(slot16Rtv.Get(), seven);
        ID3D11RenderTargetView* rtvs[2] = {hdrRtv.Get(), slot16Rtv.Get()};
        g.context->OMSetRenderTargets(2, rtvs, nullptr);
        g.context->PSSetShaderResources(127, 1, &view);
        g.context->PSSetShader(twinPs.Get(), nullptr, 0);
        g.context->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        g.context->PSSetShaderResources(127, 1, &none);
        g.context->OMSetRenderTargets(0, nullptr, nullptr);
    };
    if (twinPs) {
        twin_draw(slotView.Get());
        const auto texel = [&](ID3D11Texture2D* tex, UINT x, UINT y, UINT bytes) {
            D3D11_TEXTURE2D_DESC t;
            tex->GetDesc(&t);
            t.Usage = D3D11_USAGE_STAGING;
            t.BindFlags = 0;
            t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            g.device->CreateTexture2D(&t, nullptr, &staging);
            g.context->CopyResource(staging.Get(), tex);
            D3D11_MAPPED_SUBRESOURCE m{};
            uint64_t v = ~0ull;
            if (SUCCEEDED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                v = 0;
                std::memcpy(&v, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * bytes, bytes);
                g.context->Unmap(staging.Get(), 0);
            }
            return v;
        };
        const uint64_t inside = texel(slot16.Get(), 10, 10, 2), lower = texel(slot16.Get(), 10, 62, 2),
                       discarded = texel(slot16.Get(), 0, 10, 2);
        const bool colourSame = texel(hdr.Get(), 10, 10, 8) == texel(reference.Get(), 10, 10, 8) &&
                                texel(hdr.Get(), 10, 62, 8) == texel(reference.Get(), 10, 62, 8) &&
                                texel(hdr.Get(), 0, 10, 8) == texel(reference.Get(), 0, 10, 8);
        std::snprintf(what, sizeof(what),
                      "the twin writes the bound slot word (0x%04llx and 0x%04llx at both returns, 0x1003 bound), "
                      "nothing where it discards (%llu: as cleared)",
                      static_cast<unsigned long long>(inside), static_cast<unsigned long long>(lower),
                      static_cast<unsigned long long>(discarded));
        check(inside == 0x1003 && lower == 0x1003 && discarded == 7, what);
        check(colourSame, "target 0 exactly as the game's shader draws it (both returns, the discard)");
        twin_draw(nullptr);
        check(texel(slot16.Get(), 10, 10, 2) == 0, "with no slot view bound it writes 0 (none)");
    }
    // The twin of a twin: what texture sharpening's mip bias twin becomes when it gets the slot output too.
    const dxbc::SlotOutput again = dxbc::add_slot_output(twin.bytecode.data(), twin.bytecode.size());
    check(again.bytecode.empty(), "a shader with the slot output already: refused");

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
