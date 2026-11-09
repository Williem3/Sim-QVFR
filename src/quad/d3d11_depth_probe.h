// Reads a game's depth at a few pixels, a frame or two later, without stalling: a compute shader loads them from the
// depth texture into a small buffer, copied to one of three staging buffers and mapped once the GPU is done with it.
// Recorded into a deferred context and executed on the immediate one with the game's state restored (as
// d3d11_view_resample.h), on the thread that renders with that context. For the converging focus boxes
// (focus_converge.h): the depth where the eyes look.
#pragma once

#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

namespace simqvfr {

class D3D11DepthProbe {
public:
    static constexpr int kPoints = 9;
    bool initialize(ID3D11Device* device);
    bool ready() const { return ready_; }
    // Queues a read of `depth` (a depth texture made typeless and shader-readable: R24G8, R32G8X24 or R32; single
    // sampled) at `points` (x, y texels). False: not queued (the texture can't be read so: logged once).
    bool request(ID3D11Texture2D* depth, const int (&points)[kPoints][2]);
    // The newest finished read: the raw depth values (0-1) and the number of frames since it was queued. False: none new.
    bool latest(float (&values)[kPoints], int& age);
    void shutdown();

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    ID3D11ShaderResourceView* view_of(ID3D11Texture2D* depth);
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> immediate_, deferred_;
    Com<ID3D11ComputeShader> cs_;
    Com<ID3D11Buffer> constants_, output_;
    Com<ID3D11UnorderedAccessView> outputView_;
    static constexpr int kRing = 3;
    Com<ID3D11Buffer> staging_[kRing];
    std::uint64_t queuedAt_[kRing] = {};  // the request number each staging buffer holds (0: none)
    std::uint64_t requests_ = 0, read_ = 0;
    ID3D11Texture2D* viewTexture_ = nullptr;  // the texture `view_` reads (compared only)
    Com<ID3D11ShaderResourceView> view_;
    bool refusedLogged_ = false;
    bool ready_ = false;
};

}  // namespace simqvfr
