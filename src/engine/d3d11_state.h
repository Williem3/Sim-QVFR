// The game's D3D11 state kept across our own work mid-frame and put back (d3d11_state::Guard), shared by the engine's
// and the games' draws. Each caller names the parts it changes (Spec), so a per-draw blit makes the few calls it made
// by hand and the DLSS evaluation the full set. Header-only: LMU's per-draw path stays in one translation unit.
//
// The restore runs in one order for every caller: after the optional unbind, the input assembler, the shaders, their
// constant buffers and samplers, the rasterizer, blend and depth, then the output merger's targets, the compute UAVs,
// the shader resources last, then stream-out and the predicate. Targets before resources: a resource view put back
// never meets our own target still bound on the same texture (the runtime would null it), and once the game's targets
// are back only its own views follow, which were bound together before.
#pragma once

#include <d3d11_1.h>

#include <cstdint>

namespace simqvfr::d3d11_state {

enum Part : uint32_t {
    kInputLayout = 1u << 0,   // IA input layout and primitive topology
    kIABuffers = 1u << 1,     // IA vertex buffers (all slots) and index buffer
    kVS = 1u << 2,            // the shaders (with class instances per Spec::instances)
    kPS = 1u << 3,
    kCS = 1u << 4,
    kGsHsDs = 1u << 5,        // geometry, hull and domain shaders (never with instances)
    kOMTargets = 1u << 6,     // the 8 render targets and the depth view
    kOMUavs = 1u << 7,        // the targets with the output merger's UAVs (implies kOMTargets)
    kBlend = 1u << 8,         // blend state, factor and sample mask
    kDepth = 1u << 9,         // depth-stencil state and reference
    kRSState = 1u << 10,
    kViewports = 1u << 11,
    kScissors = 1u << 12,
    kStreamOut = 1u << 13,    // saved, cleared for our work, put back appending (offsets can't be read)
    kPredication = 1u << 14,  // saved, cleared for our work when set, put back
    kAll = (1u << 15) - 1,
};

// A shader stage's slots: [first, first + count).
struct Slots {
    UINT first = 0, count = 0;
};

struct Spec {
    uint32_t parts = 0;
    // Constant buffers keep their ranges (ID3D11DeviceContext1's *ConstantBuffers1 when the context has it).
    Slots vsCb, psCb, csCb;
    Slots vsSrv, psSrv, csSrv;  // up to kSrvs
    Slots vsSamplers, psSamplers, csSamplers;
    Slots csUavs;
    bool instances = true;    // VS/PS/CS class instances kept (false: the shader alone, as the SCS blits do)
    bool emptyRects = true;   // a saved viewport/scissor count of 0 set back (clears them); false: not set at all
    bool unbindFirst = false; // the saved targets, compute UAVs and resource slots emptied before anything is put back
};

// The setters the guard calls, straight on the context. A game whose own hooks must not see the restores derives a
// policy from this and hides the setters it routes past them (LMU's RealCalls, mvr_pass_hook_table.h).
struct DirectCalls {
    using C = ID3D11DeviceContext;
    using C1 = ID3D11DeviceContext1;
    static void IASetInputLayout(C* c, ID3D11InputLayout* l) { c->IASetInputLayout(l); }
    static void IASetPrimitiveTopology(C* c, D3D11_PRIMITIVE_TOPOLOGY t) { c->IASetPrimitiveTopology(t); }
    static void IASetVertexBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st, const UINT* o) {
        c->IASetVertexBuffers(s, n, b, st, o);
    }
    static void IASetIndexBuffer(C* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT o) { c->IASetIndexBuffer(b, f, o); }
    static void VSSetShader(C* c, ID3D11VertexShader* s, ID3D11ClassInstance* const* i, UINT n) { c->VSSetShader(s, i, n); }
    static void PSSetShader(C* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* i, UINT n) { c->PSSetShader(s, i, n); }
    static void CSSetShader(C* c, ID3D11ComputeShader* s, ID3D11ClassInstance* const* i, UINT n) { c->CSSetShader(s, i, n); }
    static void GSSetShader(C* c, ID3D11GeometryShader* s) { c->GSSetShader(s, nullptr, 0); }
    static void HSSetShader(C* c, ID3D11HullShader* s) { c->HSSetShader(s, nullptr, 0); }
    static void DSSetShader(C* c, ID3D11DomainShader* s) { c->DSSetShader(s, nullptr, 0); }
    static void VSSetConstantBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b) { c->VSSetConstantBuffers(s, n, b); }
    static void PSSetConstantBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b) { c->PSSetConstantBuffers(s, n, b); }
    static void CSSetConstantBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b) { c->CSSetConstantBuffers(s, n, b); }
    static void VSSetConstantBuffers1(C1* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) {
        c->VSSetConstantBuffers1(s, n, b, f, k);
    }
    static void PSSetConstantBuffers1(C1* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) {
        c->PSSetConstantBuffers1(s, n, b, f, k);
    }
    static void CSSetConstantBuffers1(C1* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) {
        c->CSSetConstantBuffers1(s, n, b, f, k);
    }
    static void VSSetShaderResources(C* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) { c->VSSetShaderResources(s, n, v); }
    static void PSSetShaderResources(C* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) { c->PSSetShaderResources(s, n, v); }
    static void CSSetShaderResources(C* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) { c->CSSetShaderResources(s, n, v); }
    static void VSSetSamplers(C* c, UINT s, UINT n, ID3D11SamplerState* const* v) { c->VSSetSamplers(s, n, v); }
    static void PSSetSamplers(C* c, UINT s, UINT n, ID3D11SamplerState* const* v) { c->PSSetSamplers(s, n, v); }
    static void CSSetSamplers(C* c, UINT s, UINT n, ID3D11SamplerState* const* v) { c->CSSetSamplers(s, n, v); }
    static void CSSetUnorderedAccessViews(C* c, UINT s, UINT n, ID3D11UnorderedAccessView* const* u, const UINT* k) {
        c->CSSetUnorderedAccessViews(s, n, u, k);
    }
    static void OMSetRenderTargets(C* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
        c->OMSetRenderTargets(n, r, d);
    }
    static void OMSetRenderTargetsAndUnorderedAccessViews(C* c, UINT n, ID3D11RenderTargetView* const* r,
                                                          ID3D11DepthStencilView* d, UINT us, UINT un,
                                                          ID3D11UnorderedAccessView* const* u, const UINT* k) {
        c->OMSetRenderTargetsAndUnorderedAccessViews(n, r, d, us, un, u, k);
    }
    static void OMSetBlendState(C* c, ID3D11BlendState* b, const FLOAT f[4], UINT m) { c->OMSetBlendState(b, f, m); }
    static void OMSetDepthStencilState(C* c, ID3D11DepthStencilState* d, UINT r) { c->OMSetDepthStencilState(d, r); }
    static void RSSetState(C* c, ID3D11RasterizerState* r) { c->RSSetState(r); }
    static void RSSetViewports(C* c, UINT n, const D3D11_VIEWPORT* v) { c->RSSetViewports(n, v); }
    static void RSSetScissorRects(C* c, UINT n, const D3D11_RECT* r) { c->RSSetScissorRects(n, r); }
    static void SOSetTargets(C* c, UINT n, ID3D11Buffer* const* b, const UINT* o) { c->SOSetTargets(n, b, o); }
    static void SetPredication(C* c, ID3D11Predicate* p, BOOL v) { c->SetPredication(p, v); }
};

// Reads the parts at construction (and clears stream-out and the predicate when asked), puts them back at
// destruction, each Get released once after its Set. The arrays are left unset: only the slots read are touched.
template <class Calls = DirectCalls>
class Guard {
public:
    // (Slot counts named apart from the parts: a member kStreamOut would hide the part bit inside the class.)
    static constexpr UINT kBuffers = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, kCbs = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT,
                          kSrvs = 32, kSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, kUavs = D3D11_PS_CS_UAV_REGISTER_COUNT,
                          kTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, kSoSlots = D3D11_SO_BUFFER_SLOT_COUNT,
                          kInstances = 256, kRects = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    // A plain context: asked for ID3D11DeviceContext1 only when constant buffers are kept (their ranges).
    Guard(ID3D11DeviceContext* c, const Spec& spec) : c_(c), s_(clamped(spec)) {
        if (s_.vsCb.count || s_.psCb.count || s_.csCb.count) {
            c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1_));
            ownC1_ = c1_ != nullptr;
        }
        save();
    }
    // A D3D11.1 context (the SCS games'): no QueryInterface.
    Guard(ID3D11DeviceContext1* c, const Spec& spec) : c_(c), c1_(c), s_(clamped(spec)) { save(); }
    ~Guard() { restore(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    // What was read (valid while the guard lives, for parts asked for).
    ID3D11RenderTargetView* rtv(UINT i) const { return rtv_[i]; }
    ID3D11DepthStencilView* dsv() const { return dsv_; }
    UINT viewport_count() const { return viewports_; }
    const D3D11_VIEWPORT* viewports() const { return vp_; }
    UINT scissor_count() const { return scissors_; }
    const D3D11_RECT* scissors() const { return sc_; }

private:
    static Spec clamped(Spec s) {
        if (s.parts & kOMUavs)
            s.parts |= kOMTargets;
        const auto clamp = [](Slots& r, UINT n) {
            r.first = r.first < n ? r.first : n;
            r.count = r.count < n - r.first ? r.count : n - r.first;
        };
        clamp(s.vsCb, kCbs), clamp(s.psCb, kCbs), clamp(s.csCb, kCbs);
        clamp(s.vsSrv, kSrvs), clamp(s.psSrv, kSrvs), clamp(s.csSrv, kSrvs);
        clamp(s.vsSamplers, kSamplers), clamp(s.psSamplers, kSamplers), clamp(s.csSamplers, kSamplers);
        clamp(s.csUavs, kUavs);
        return s;
    }
    template <class T> static void release(T*& p) {
        if (p)
            p->Release();
        p = nullptr;
    }
    template <class T> static void release(T** a, Slots r) {
        for (UINT i = r.first; i < r.first + r.count; ++i)
            release(a[i]);
    }
    template <class T> static void release_instances(T** a, UINT n) {
        for (UINT i = 0; i < n && i < kInstances; ++i)
            release(a[i]);
    }

    void save() {
        ID3D11DeviceContext* c = c_;
        const uint32_t p = s_.parts;
        if (p & kInputLayout) {
            c->IAGetInputLayout(&layout_);
            c->IAGetPrimitiveTopology(&topology_);
        }
        if (p & kIABuffers) {
            c->IAGetVertexBuffers(0, kBuffers, vb_, strides_, offsets_);
            c->IAGetIndexBuffer(&ib_, &ibFormat_, &ibOffset_);
        }
        const bool inst = s_.instances;
        if (p & kVS) {
            vsN_ = inst ? kInstances : 0;
            c->VSGetShader(&vs_, inst ? vsI_ : nullptr, inst ? &vsN_ : nullptr);
        }
        if (p & kPS) {
            psN_ = inst ? kInstances : 0;
            c->PSGetShader(&ps_, inst ? psI_ : nullptr, inst ? &psN_ : nullptr);
        }
        if (p & kCS) {
            csN_ = inst ? kInstances : 0;
            c->CSGetShader(&cs_, inst ? csI_ : nullptr, inst ? &csN_ : nullptr);
        }
        if (p & kGsHsDs) {
            c->GSGetShader(&gs_, nullptr, nullptr);
            c->HSGetShader(&hs_, nullptr, nullptr);
            c->DSGetShader(&ds_, nullptr, nullptr);
        }
        if (const Slots r = s_.vsCb; r.count)
            c1_ ? c1_->VSGetConstantBuffers1(r.first, r.count, vsCb_ + r.first, vsFirst_ + r.first, vsNum_ + r.first)
                : c->VSGetConstantBuffers(r.first, r.count, vsCb_ + r.first);
        if (const Slots r = s_.psCb; r.count)
            c1_ ? c1_->PSGetConstantBuffers1(r.first, r.count, psCb_ + r.first, psFirst_ + r.first, psNum_ + r.first)
                : c->PSGetConstantBuffers(r.first, r.count, psCb_ + r.first);
        if (const Slots r = s_.csCb; r.count)
            c1_ ? c1_->CSGetConstantBuffers1(r.first, r.count, csCb_ + r.first, csFirst_ + r.first, csNum_ + r.first)
                : c->CSGetConstantBuffers(r.first, r.count, csCb_ + r.first);
        if (const Slots r = s_.vsSrv; r.count)
            c->VSGetShaderResources(r.first, r.count, vsSrv_ + r.first);
        if (const Slots r = s_.psSrv; r.count)
            c->PSGetShaderResources(r.first, r.count, psSrv_ + r.first);
        if (const Slots r = s_.csSrv; r.count)
            c->CSGetShaderResources(r.first, r.count, csSrv_ + r.first);
        if (const Slots r = s_.vsSamplers; r.count)
            c->VSGetSamplers(r.first, r.count, vsSamp_ + r.first);
        if (const Slots r = s_.psSamplers; r.count)
            c->PSGetSamplers(r.first, r.count, psSamp_ + r.first);
        if (const Slots r = s_.csSamplers; r.count)
            c->CSGetSamplers(r.first, r.count, csSamp_ + r.first);
        if (const Slots r = s_.csUavs; r.count)
            c->CSGetUnorderedAccessViews(r.first, r.count, csUav_ + r.first);
        if (p & kOMUavs)
            c->OMGetRenderTargetsAndUnorderedAccessViews(kTargets, rtv_, &dsv_, 0, kUavs, omUav_);
        else if (p & kOMTargets)
            c->OMGetRenderTargets(kTargets, rtv_, &dsv_);
        if (p & kBlend)
            c->OMGetBlendState(&blend_, blendFactor_, &sampleMask_);
        if (p & kDepth)
            c->OMGetDepthStencilState(&depthState_, &stencilRef_);
        if (p & kStreamOut)
            c->SOGetTargets(kSoSlots, so_);
        if (p & kPredication)
            c->GetPredication(&predicate_, &predicateValue_);
        if (p & kRSState)
            c->RSGetState(&raster_);
        if (p & kViewports) {
            viewports_ = kRects;
            c->RSGetViewports(&viewports_, vp_);
        }
        if (p & kScissors) {
            scissors_ = kRects;
            c->RSGetScissorRects(&scissors_, sc_);
        }
        // Neither the game's predicate nor its stream-out targets may skip or capture our draws.
        if ((p & kPredication) && predicate_)
            Calls::SetPredication(c, nullptr, FALSE);
        if (p & kStreamOut) {
            ID3D11Buffer* noSo[kSoSlots] = {};
            Calls::SOSetTargets(c, kSoSlots, noSo, nullptr);
        }
    }

    void restore() {
        ID3D11DeviceContext* c = c_;
        const uint32_t p = s_.parts;
        if (s_.unbindFirst) {  // ours off first, so nothing put back meets a texture still bound the other way
            ID3D11ShaderResourceView* noSrv[kSrvs] = {};
            ID3D11UnorderedAccessView* noUav[kUavs] = {};
            if (p & kOMUavs)
                Calls::OMSetRenderTargetsAndUnorderedAccessViews(c, 0, nullptr, nullptr, 0, kUavs, noUav, nullptr);
            else if (p & kOMTargets)
                Calls::OMSetRenderTargets(c, 0, nullptr, nullptr);
            if (const Slots r = s_.csUavs; r.count)
                Calls::CSSetUnorderedAccessViews(c, r.first, r.count, noUav, nullptr);
            if (const Slots r = s_.vsSrv; r.count)
                Calls::VSSetShaderResources(c, r.first, r.count, noSrv);
            if (const Slots r = s_.psSrv; r.count)
                Calls::PSSetShaderResources(c, r.first, r.count, noSrv);
            if (const Slots r = s_.csSrv; r.count)
                Calls::CSSetShaderResources(c, r.first, r.count, noSrv);
        }
        if (p & kInputLayout) {
            Calls::IASetInputLayout(c, layout_);
            Calls::IASetPrimitiveTopology(c, topology_);
        }
        if (p & kIABuffers) {
            Calls::IASetVertexBuffers(c, 0, kBuffers, vb_, strides_, offsets_);
            Calls::IASetIndexBuffer(c, ib_, ibFormat_, ibOffset_);
        }
        if (p & kVS)
            Calls::VSSetShader(c, vs_, vsN_ ? vsI_ : nullptr, vsN_);
        if (p & kPS)
            Calls::PSSetShader(c, ps_, psN_ ? psI_ : nullptr, psN_);
        if (p & kCS)
            Calls::CSSetShader(c, cs_, csN_ ? csI_ : nullptr, csN_);
        if (p & kGsHsDs) {
            Calls::GSSetShader(c, gs_);
            Calls::HSSetShader(c, hs_);
            Calls::DSSetShader(c, ds_);
        }
        if (const Slots r = s_.vsCb; r.count)
            c1_ ? Calls::VSSetConstantBuffers1(c1_, r.first, r.count, vsCb_ + r.first, vsFirst_ + r.first, vsNum_ + r.first)
                : Calls::VSSetConstantBuffers(c, r.first, r.count, vsCb_ + r.first);
        if (const Slots r = s_.psCb; r.count)
            c1_ ? Calls::PSSetConstantBuffers1(c1_, r.first, r.count, psCb_ + r.first, psFirst_ + r.first, psNum_ + r.first)
                : Calls::PSSetConstantBuffers(c, r.first, r.count, psCb_ + r.first);
        if (const Slots r = s_.csCb; r.count)
            c1_ ? Calls::CSSetConstantBuffers1(c1_, r.first, r.count, csCb_ + r.first, csFirst_ + r.first, csNum_ + r.first)
                : Calls::CSSetConstantBuffers(c, r.first, r.count, csCb_ + r.first);
        if (const Slots r = s_.vsSamplers; r.count)
            Calls::VSSetSamplers(c, r.first, r.count, vsSamp_ + r.first);
        if (const Slots r = s_.psSamplers; r.count)
            Calls::PSSetSamplers(c, r.first, r.count, psSamp_ + r.first);
        if (const Slots r = s_.csSamplers; r.count)
            Calls::CSSetSamplers(c, r.first, r.count, csSamp_ + r.first);
        if (p & kRSState)
            Calls::RSSetState(c, raster_);
        if ((p & kViewports) && (viewports_ || s_.emptyRects))
            Calls::RSSetViewports(c, viewports_, viewports_ ? vp_ : nullptr);
        if ((p & kScissors) && (scissors_ || s_.emptyRects))
            Calls::RSSetScissorRects(c, scissors_, scissors_ ? sc_ : nullptr);
        if (p & kBlend)
            Calls::OMSetBlendState(c, blend_, blendFactor_, sampleMask_);
        if (p & kDepth)
            Calls::OMSetDepthStencilState(c, depthState_, stencilRef_);
        if (p & kOMUavs) {
            // The output merger's UAVs take the slots after its render targets (UAVStartSlot >= NumRTVs).
            UINT targets = 0;
            for (UINT i = 0; i < kTargets; ++i)
                if (rtv_[i])
                    targets = i + 1;
            const UINT uavStart = targets < kUavs ? targets : kUavs;
            Calls::OMSetRenderTargetsAndUnorderedAccessViews(c, targets, rtv_, dsv_, uavStart, kUavs - uavStart,
                                                             omUav_ + uavStart, nullptr);
        } else if (p & kOMTargets) {
            Calls::OMSetRenderTargets(c, kTargets, rtv_, dsv_);
        }
        if (const Slots r = s_.csUavs; r.count)
            Calls::CSSetUnorderedAccessViews(c, r.first, r.count, csUav_ + r.first, nullptr);
        if (const Slots r = s_.vsSrv; r.count)
            Calls::VSSetShaderResources(c, r.first, r.count, vsSrv_ + r.first);
        if (const Slots r = s_.psSrv; r.count)
            Calls::PSSetShaderResources(c, r.first, r.count, psSrv_ + r.first);
        if (const Slots r = s_.csSrv; r.count)
            Calls::CSSetShaderResources(c, r.first, r.count, csSrv_ + r.first);
        if (p & kStreamOut) {
            // Stream-out offsets can't be read back: -1 appends where each buffer's filled size left off.
            UINT soOffsets[kSoSlots];
            for (UINT& o : soOffsets)
                o = static_cast<UINT>(-1);
            Calls::SOSetTargets(c, kSoSlots, so_, soOffsets);
        }
        if (p & kPredication)
            Calls::SetPredication(c, predicate_, predicateValue_);

        release(layout_), release(ib_), release(vs_), release(ps_), release(cs_), release(gs_), release(hs_), release(ds_);
        release(dsv_), release(blend_), release(depthState_), release(raster_), release(predicate_);
        if (p & kIABuffers)
            release(vb_, {0, kBuffers});
        release(vsCb_, s_.vsCb), release(psCb_, s_.psCb), release(csCb_, s_.csCb);
        release(vsSrv_, s_.vsSrv), release(psSrv_, s_.psSrv), release(csSrv_, s_.csSrv);
        release(vsSamp_, s_.vsSamplers), release(psSamp_, s_.psSamplers), release(csSamp_, s_.csSamplers);
        release(csUav_, s_.csUavs);
        if (p & kOMUavs)
            release(omUav_, {0, kUavs});
        if (p & kOMTargets)
            release(rtv_, {0, kTargets});
        if (p & kStreamOut)
            release(so_, {0, kSoSlots});
        release_instances(vsI_, vsN_), release_instances(psI_, psN_), release_instances(csI_, csN_);
        if (ownC1_)
            c1_->Release();
    }

    ID3D11DeviceContext* c_;
    ID3D11DeviceContext1* c1_ = nullptr;
    bool ownC1_ = false;
    Spec s_;
    // Single objects start null (released unconditionally); arrays are written by the Gets before any use.
    ID3D11InputLayout* layout_ = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vb_[kBuffers];
    UINT strides_[kBuffers], offsets_[kBuffers];
    ID3D11Buffer* ib_ = nullptr;
    DXGI_FORMAT ibFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset_ = 0;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11ComputeShader* cs_ = nullptr;
    ID3D11GeometryShader* gs_ = nullptr;
    ID3D11HullShader* hs_ = nullptr;
    ID3D11DomainShader* ds_ = nullptr;
    ID3D11ClassInstance* vsI_[kInstances];
    ID3D11ClassInstance* psI_[kInstances];
    ID3D11ClassInstance* csI_[kInstances];
    UINT vsN_ = 0, psN_ = 0, csN_ = 0;
    ID3D11Buffer* vsCb_[kCbs];
    ID3D11Buffer* psCb_[kCbs];
    ID3D11Buffer* csCb_[kCbs];
    UINT vsFirst_[kCbs], vsNum_[kCbs], psFirst_[kCbs], psNum_[kCbs], csFirst_[kCbs], csNum_[kCbs];
    ID3D11ShaderResourceView* vsSrv_[kSrvs];
    ID3D11ShaderResourceView* psSrv_[kSrvs];
    ID3D11ShaderResourceView* csSrv_[kSrvs];
    ID3D11SamplerState* vsSamp_[kSamplers];
    ID3D11SamplerState* psSamp_[kSamplers];
    ID3D11SamplerState* csSamp_[kSamplers];
    ID3D11UnorderedAccessView* csUav_[kUavs];
    ID3D11UnorderedAccessView* omUav_[kUavs];
    ID3D11RenderTargetView* rtv_[kTargets];
    ID3D11DepthStencilView* dsv_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    FLOAT blendFactor_[4] = {};
    UINT sampleMask_ = 0xffffffff;
    ID3D11DepthStencilState* depthState_ = nullptr;
    UINT stencilRef_ = 0;
    ID3D11Buffer* so_[kSoSlots];
    ID3D11Predicate* predicate_ = nullptr;
    BOOL predicateValue_ = FALSE;
    ID3D11RasterizerState* raster_ = nullptr;
    UINT viewports_ = 0, scissors_ = 0;
    D3D11_VIEWPORT vp_[kRects];
    D3D11_RECT sc_[kRects];
};

}  // namespace simqvfr::d3d11_state

namespace simqvfr {
using D3D11StateGuard = d3d11_state::Guard<>;
}  // namespace simqvfr
