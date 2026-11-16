// A small D3D12 device of ours beside a D3D11 game's, for a runtime that works on D3D12 only: DLSS-NR (engine/ngx_nr.cpp)
// and AMD FSR (engine/fsr_upscale.cpp) each own one. A view's inputs and output are textures both devices share (made
// on D3D12, opened on the game's device); a shared fence orders the work on the GPU without the CPU waiting: D3D11 fills
// the inputs and signals, the D3D12 queue waits, runs the backend's work and signals, D3D11 waits before it reads the
// output. Released views are destroyed only once both devices are through everything queued that touches them (a second
// fence the game's device signals), never under queued work (engine/ngx_nr.h release()).
// The backend keeps its own runtime's calls (start, a view's feature or context, its evaluation, destroy); the bridge
// calls the backend's destroy hook for its views.
#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace simqvfr {

// What the bridge keeps of a view: its four shared textures, its timing, and its release.
struct BridgeView {
    Microsoft::WRL::ComPtr<ID3D12Resource> colour, depth, vectors, output;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> colour11, depth11, vectors11, output11;
    // Its evaluations' GPU time: two timestamps a submission, read once that submission is through.
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::ComPtr<ID3D12Resource> times;
    UINT64 timedAt = 0;                 // the fence value of the submission whose timestamps `times` holds (0: none)
    UINT64 lastSubmit = 0;              // the fence value its last submission is through at
    unsigned long long releasedAt = 0;  // collect()'s count when it was released
    UINT64 releaseValue = 0;            // the release fence's value the game's device signals once it is through with it
    std::vector<Microsoft::WRL::ComPtr<IUnknown>> keep;  // the caller's Direct3D 11 objects on its textures, let go of with it
    double msSum = 0;
    unsigned msCount = 0;
};

class D3D12Bridge {
public:
    // `tag`: the log lines' prefix ("ngx nr"); `outputName`: the output texture in them ("the model's output");
    // `destroy`: the backend's own release of a view's runtime objects, then `delete` of the view.
    using Destroy = void (*)(BridgeView*);
    D3D12Bridge(const char* tag, const char* outputName, Destroy destroy) : tag_(tag), outputName_(outputName), destroy_(destroy) {}
    D3D12Bridge(const D3D12Bridge&) = delete;
    D3D12Bridge& operator=(const D3D12Bridge&) = delete;

    // The session's state, the backend's to read and set: start() answers as its first call did while `tried`.
    bool started = false, tried = false;
    std::string failure;
    // `why` and `failure` set to "what (0xhr)"; false.
    bool fail(std::string& why, const char* what, HRESULT hr);

    // The game's device as ID3D11Device5 (shared fences) and its adapter. False with the reason.
    bool open_game_device(ID3D11Device* device, Microsoft::WRL::ComPtr<IDXGIAdapter>& adapter, std::string& why);
    // Our device on `adapter`, its queue and the two shared fences opened on the game's device. False with the reason
    // (the queue's and fences' failures release what was made; a failed device creation leaves the game's device held,
    // as before, until shutdown()).
    bool create_device(IDXGIAdapter* adapter, std::string& why);

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12GraphicsCommandList* list() const { return list_.Get(); }
    // Our device, given up by the bridge (a backend that must release it before the rest: ngx_nr.cpp's failed init).
    Microsoft::WRL::ComPtr<ID3D12Device> take_device() { return std::move(device_); }
    UINT64 completed() const { return fence_->GetCompletedValue(); }
    UINT64 last_value() const { return value_; }

    // An allocator no submission still uses, its list ready to record.
    bool begin_list();
    // The list submitted; the fence signalled with a new value after it (returned; 0: the submission failed).
    UINT64 end_list();
    bool wait_for(UINT64 value, DWORD ms);
    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

    // A view's four shared textures (colour, depth R32F, vectors RG16F at w x h; output at outW x outH) and its timestamp
    // queries. False with Direct3D's result in `hr` (the failed texture logged).
    bool make_view(BridgeView* v, unsigned w, unsigned h, DXGI_FORMAT colour, unsigned outW, unsigned outH, DXGI_FORMAT output,
                   HRESULT& hr);

    // An evaluation's start: our device alive, the backlog under `backlog` (else `result` = `behind`), a list begun, the
    // queue waiting for D3D11's work so far, the textures in the runtime's states, the first timestamp. False: nothing
    // submitted (`result` set for the first two; a list that couldn't begin leaves it).
    struct Evaluating {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
        bool timed = false;
    };
    bool begin_evaluation(ID3D11DeviceContext* context, BridgeView* v, size_t backlog, unsigned behind, unsigned& result,
                          Evaluating& e);
    // Its end, whatever the runtime answered (the queue's wait must be followed by a signal D3D11 can wait for): the
    // second timestamp, the textures back, the list submitted, D3D11 waiting for it. False: the submission failed.
    bool end_evaluation(BridgeView* v, Evaluating& e);

    // A view that never came into use: destroyed now if our queue is through it, else parked with the released ones.
    // Never waits: the render thread must not block on the GPU.
    void discard(BridgeView* v);
    // Parked with the released ones, the game's device never having touched it.
    void park(BridgeView* v);
    void release(ID3D11DeviceContext* context, BridgeView* v, IUnknown* const* keep, unsigned count);
    // collect(): the frame counted, then the released views whose work both devices finished destroyed. In two steps
    // for a backend with work of its own between them.
    unsigned long long count_frame() { return ++collects_; }
    unsigned long long frames() const { return collects_; }
    void destroy_finished();
    void collect() {
        count_frame();
        destroy_finished();
    }
    unsigned released() const { return static_cast<unsigned>(released_.size()); }

    bool device_lost() const { return device_ && FAILED(device_->GetDeviceRemovedReason()); }
    double take_gpu_ms(BridgeView* v);

    // The session's end on the game's immediate `context`: flushed, and both devices given 3 s each to finish. True:
    // they did, the released views are destroyed, and the device is still held for the backend's own shutdown (then
    // reset()). False: nothing is destroyed, our objects are left held to the process's end (logged with `work` and
    // `leftNote`) and the bridge is reset.
    bool shutdown(ID3D11DeviceContext* context, const char* work, const char* leftNote);
    // Everything released and forgotten (started, tried, failure too).
    void reset();

private:
    void collect_time(BridgeView* v);
    bool shared(unsigned w, unsigned h, DXGI_FORMAT format, bool output, Microsoft::WRL::ComPtr<ID3D12Resource>& r12,
                Microsoft::WRL::ComPtr<ID3D11Texture2D>& r11, HRESULT& hr);
    void clear();  // the objects forgotten, with no release of the event

    struct Allocator {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        UINT64 busyUntil = 0;  // the fence value its last list is through at
    };

    const char* tag_;
    const char* outputName_;
    Destroy destroy_;
    Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;  // closed between uses
    std::vector<Allocator> allocators_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
    UINT64 value_ = 0;
    // Releases: signalled by the game's device alone, after all it has queued (release()).
    Microsoft::WRL::ComPtr<ID3D12Fence> releaseFence_;
    Microsoft::WRL::ComPtr<ID3D11Fence> releaseFence11_;
    UINT64 releaseValue_ = 0;
    HANDLE event_ = nullptr;
    UINT64 timestampFrequency_ = 0;
    std::vector<BridgeView*> released_;  // given up, not destroyed yet (collect)
    unsigned long long collects_ = 0;
    std::deque<UINT64> outstanding_;     // the evaluations' fence values the GPU hasn't reached yet
};

}  // namespace simqvfr
