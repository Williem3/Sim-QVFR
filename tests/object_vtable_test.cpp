// Offline check of engine/object_vtable.h on a class of this test and on a WARP device: an object pointed at a vtable
// copy with some entries ours. The replaced calls reach ours and the originals still work; the module's code and
// vtable stay as they were; what it refuses; another vtable pointer written later is noticed; the device stays on its
// copy when its context's multithread protection goes on (d3d11 rewrites the context's vtable then, not the device's).
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "engine/object_vtable.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

struct Thing {
    virtual ~Thing() = default;
    virtual int a() { return 1; }
    virtual int b() { return 2; }
};

using ThingFn = int (*)(Thing*);
ThingFn originalB = nullptr;
int replaced_b(Thing* self) { return 10 * originalB(self); }

using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                         ID3D11VertexShader**);
CreateVertexShaderFn originalCreateVertexShader = nullptr;
int createVertexShaderCalls = 0;
HRESULT STDMETHODCALLTYPE create_vertex_shader(ID3D11Device* device, const void* code, SIZE_T size,
                                               ID3D11ClassLinkage* linkage, ID3D11VertexShader** shader) {
    ++createVertexShaderCalls;
    return originalCreateVertexShader(device, code, size, linkage, shader);
}

void** vtable_of(const void* object) { return *static_cast<void** const*>(object); }

}  // namespace

int main() {
    char what[200];
    std::snprintf(what, sizeof(what), "this SDK's newest interfaces: ID3D11Device5 %zu entries, ID3D11DeviceContext4 %zu",
                  simqvfr::kD3D11DeviceSlots, simqvfr::kD3D11ContextSlots);
    // More than ID3D11Device's 43 (IUnknown's 3 and its 40) and ID3D11DeviceContext's 115 (with ID3D11DeviceChild's 7).
    check(simqvfr::kD3D11DeviceSlots > 43 && simqvfr::kD3D11ContextSlots > 115, what);

    // A class of ours: the replaced entry calls ours, ours the original; the others as before.
    {
        Thing thing;
        Thing* volatile p = &thing;  // called through the vtable, not inlined
        void** own = vtable_of(&thing);
        simqvfr::ObjectVtable v;
        check(v.install(&thing, 3, {{2, reinterpret_cast<void*>(&replaced_b), reinterpret_cast<void**>(&originalB)}}),
              "installed on an object of a class with three virtual functions");
        check(vtable_of(&thing) != own && v.installed() && v.slots() >= 3, "the object on the copy, three entries or more");
        check(p->a() == 1 && p->b() == 20, "the replaced entry reaches ours, ours the original; the other one as before");
        check(!v.install(&thing, 3, {}), "installed once only");
        void** copy = vtable_of(&thing);
        *reinterpret_cast<void***>(&thing) = own;
        check(!v.installed(), "another vtable pointer written later is noticed");
        *reinterpret_cast<void***>(&thing) = copy;
        v.remove();
        check(vtable_of(&thing) == own && !v.installed() && p->b() == 2, "removed: the object on its own vtable again");

        simqvfr::ObjectVtable w;
        check(!w.install(&thing, 1000, {}), "refused: fewer entries than the caller needs");
        check(!w.install(&thing, 3, {{50, reinterpret_cast<void*>(&replaced_b), reinterpret_cast<void**>(&originalB)}}),
              "refused: an entry beyond the vtable");
        check(vtable_of(&thing) == own && !w.installed(), "refusals change nothing");
        static void* notCode[4] = {&failures, &failures, &failures, &failures};
        struct Fake {
            void** vptr;
        } fake = {notCode + 1};
        check(!w.install(&fake, 1, {}) && fake.vptr == notCode + 1, "refused: a vtable that holds no code");
        check(!w.install(nullptr, 1, {}), "refused: no object");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("FAIL no WARP device\n");
        return 1;
    }

    // The device: on a copy, CreateVertexShader (slot 12) ours, the shader still created; d3d11.dll's code as it was.
    {
        void** own = vtable_of(device.Get());
        void* const ownFn = own[12];
        uint8_t code[16];
        std::memcpy(code, ownFn, sizeof(code));
        simqvfr::ObjectVtable v;
        check(v.install(device.Get(), simqvfr::kD3D11DeviceSlots,
                        {{12, reinterpret_cast<void*>(&create_vertex_shader),
                          reinterpret_cast<void**>(&originalCreateVertexShader)}}),
              "installed on the device");
        const std::string source = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
        ComPtr<ID3DBlob> blob, errors;
        const bool compiled = SUCCEEDED(D3DCompile(source.data(), source.size(), "t", nullptr, nullptr, "main", "vs_5_0",
                                                   0, 0, &blob, &errors));
        const auto create = [&] {
            ComPtr<ID3D11VertexShader> vs;
            return compiled &&
                   SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs)) &&
                   vs;
        };
        check(create() && createVertexShaderCalls == 1, "the device's shader creation reaches ours, the shader created");
        ComPtr<ID3D11Device5> device5;
        check(SUCCEEDED(device.As(&device5)) && static_cast<ID3D11Device*>(device5.Get()) == device.Get(),
              "ID3D11Device5 is the same pointer, on the same copy");
        check(vtable_of(device.Get()) != own && own[12] == ownFn && std::memcmp(code, ownFn, sizeof(code)) == 0 &&
                  SUCCEEDED(device->GetDeviceRemovedReason()),
              "the device fine on the copy; d3d11.dll's vtable and CreateVertexShader code as they were");
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(context.As(&mt))) {
            const BOOL was = mt->SetMultithreadProtected(TRUE);
            check(v.installed() && create() && createVertexShaderCalls == 2,
                  "multithread protection on: the device still on the copy, its shader creation still ours");
            mt->SetMultithreadProtected(was);
        } else {
            check(false, "the context's multithread protection");
        }
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
