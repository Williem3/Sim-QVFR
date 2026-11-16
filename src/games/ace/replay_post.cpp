// The focus views' post (replay_internal.h has the file map): MSAA resolves, our DLSS per focus view, MSAA
// antialiasing without DLSS, and bloom / tonemap / fxaa, recorded at the Close of the game's post lists.
#include "replay_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../../log.h"
#include "replay_shaders.h"

namespace simqvfr::replay_detail {

// ---- MSAA: the focus views' depth and velocity resolves ---------------------------------------------------------

// At the game's Close of its resolve list (resolveDepthCompute, resolveDepthVelocityCompute: per pixel, no constants,
// single-descriptor tables): ACE's two pipelines replayed on our multisampled depth and velocity twins into our
// single-sample focus depth (stand-in for depthResolveTarget), closest depth and velocity (velocity_n_reactive_mask).
void focus_resolve_record(List* gameList, const Stream& s) {
    const Stream::TileDispatch *rd = nullptr, *rdv = nullptr;
    for (const auto& d : s.tileDispatches) {
        if (d.kind == 14 && !rd)
            rd = &d;
        if (d.kind == 15 && !rdv)
            rdv = &d;
    }
    if (!rd)
        return;
    Replay* r = nullptr;
    std::string why;
    UINT w = 0, h = 0;
    ID3D12Resource *msDepth = nullptr, *msVelocity = nullptr;
    AcquireSRWLockExclusive(&focus.lock);
    const Twin *depthTwin = nullptr, *velocityTwin = nullptr;
    for (const auto& [g, tw] : focus.twins) {
        if (tw.gameDesc.SampleDesc.Count < 2)
            continue;
        if (tw.depth && tw.name == "msaaDepth")
            depthTwin = &tw;
        else if (!tw.depth && tw.name.find("velocity") != std::string::npos)
            velocityTwin = &tw;
    }
    if (focus.resolveBroken || !focus.gameHeap || !focus.slotFirst || !focus.fogUavFirst || !depthTwin) {
        why = "the multisampled depth twin, our bindless block or our UAV block (fog) not there";
    } else {
        w = 2 * depthTwin->focusW;
        h = depthTwin->focusH;
        msDepth = depthTwin->focus.Get();
        msVelocity = velocityTwin ? velocityTwin->focus.Get() : nullptr;
        if (focus.resolveDepthSource != msDepth || focus.resolveVelocitySource != msVelocity) {
            for (ID3D12Resource* old : {focus.resolvedDepth.Get(), focus.closestDepth.Get(), focus.resolvedVelocity.Get()})
                retire(old);
            focus.resolvedDepth.Reset();
            focus.closestDepth.Reset();
            focus.resolvedVelocity.Reset();
            focus.resolveReady = false;
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
            const D3D12_RESOURCE_STATES read =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            const auto make = [&](DXGI_FORMAT f, ComPtr<ID3D12Resource>& out) {
                d.Format = f;
                return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, read, nullptr,
                                                                 IID_PPV_ARGS(&out)));
            };
            const DXGI_FORMAT velFormat = velocityTwin ? velocityTwin->viewFormat : DXGI_FORMAT_R16G16_FLOAT;
            if (!make(DXGI_FORMAT_R32_FLOAT, focus.resolvedDepth) || !make(DXGI_FORMAT_R32_FLOAT, focus.closestDepth) ||
                (velocityTwin && !make(velFormat, focus.resolvedVelocity))) {
                log_line("replay focus: msaa: could not make our resolved depth / velocity; no focus resolves");
                focus.resolveBroken = true;
            } else {
                inReplay = true;
                D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                v.Format = DXGI_FORMAT_R32_FLOAT;
                srvViewOriginal(device.Get(), msDepth, &v, game_cpu(focus.slotFirst + 74));
                if (msVelocity) {
                    v.Format = velFormat;
                    srvViewOriginal(device.Get(), msVelocity, &v, game_cpu(focus.slotFirst + 75));
                }
                const auto uav = [&](ID3D12Resource* res, UINT offset) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
                    u.Format = res->GetDesc().Format;
                    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    uavViewOriginal(device.Get(), res, nullptr, &u, game_cpu(focus.fogUavFirst + offset));
                };
                uav(focus.resolvedDepth.Get(), 29);
                uav(focus.closestDepth.Get(), 30);
                if (focus.resolvedVelocity)
                    uav(focus.resolvedVelocity.Get(), 31);
                inReplay = false;
                focus.resolvedDepthTwin = *depthTwin;
                focus.resolvedDepthTwin.focus = focus.resolvedDepth;
                focus.resolvedDepthTwin.gameDesc.SampleDesc = {1, 0};
                focus.resolvedDepthTwin.gameDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                focus.resolvedDepthTwin.viewFormat = DXGI_FORMAT_R32_FLOAT;
                focus.resolvedDepthTwin.name = "depthResolveTarget";
                if (velocityTwin) {
                    focus.resolvedVelocityTwin = *velocityTwin;
                    focus.resolvedVelocityTwin.focus = focus.resolvedVelocity;
                    focus.resolvedVelocityTwin.gameDesc.SampleDesc = {1, 0};
                    focus.resolvedVelocityTwin.name = "velocity_n_reactive_mask";
                }
                focus.resolveDepthSource = msDepth;
                focus.resolveVelocitySource = msVelocity;
                logd("replay focus: msaa: our resolved focus depth / closest depth%s %ux%u (from the %u-sample twins%s)",
                     velocityTwin ? " / velocity" : "", w, h, depthTwin->gameDesc.SampleDesc.Count,
                     velocityTwin ? (", velocity " + velocityTwin->name).c_str() : ", no velocity twin");
            }
        }
        if (!focus.resolveBroken && focus.resolvedDepth)
            r = acquire_replay();
    }
    static int missLogged = 0;
    if (!r && !why.empty() && missLogged < 3) {
        ++missLogged;
        logd("replay focus: msaa: resolves not replayed (%s)", why.c_str());
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepResolve);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier(l, msDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(l, msVelocity, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (ID3D12Resource* res : {focus.resolvedDepth.Get(), focus.closestDepth.Get(), focus.resolvedVelocity.Get()})
        barrier(l, res, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->SetComputeRootSignature(rd->root);
    l->SetPipelineState(rd->pso);
    l->SetComputeRootDescriptorTable(0, game_gpu(focus.slotFirst + 74));
    l->SetComputeRootDescriptorTable(1, game_gpu(focus.fogUavFirst + 29));
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    if (rdv && msVelocity && focus.resolvedVelocity) {
        l->SetComputeRootSignature(rdv->root);
        l->SetPipelineState(rdv->pso);
        l->SetComputeRootDescriptorTable(0, game_gpu(focus.slotFirst + 74));
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 75));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.fogUavFirst + 30));
        l->SetComputeRootDescriptorTable(3, game_gpu(focus.fogUavFirst + 31));
        l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
    for (ID3D12Resource* res : {focus.resolvedDepth.Get(), focus.closestDepth.Get(), focus.resolvedVelocity.Get()})
        barrier(l, res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    barrier(l, msVelocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(l, msDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    add_pending(gameList, r);
    if (!focus.resolveReady)
        logd("replay focus: msaa: the first focus depth / velocity resolve recorded");
    focus.resolveReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.resolveReplays.fetch_add(1, std::memory_order_relaxed);
}

namespace {
// ---- P2a: our DLSS per focus view ---------------------------------------------------------------------------------

// The focus twin of a resource ACE's DLSS evaluation names, by its name (none: nullptr).
ID3D12Resource* twin_named(const std::string& name, bool depth) {  // under focus.lock
    if (depth) {
        const Twin* d = scene_depth_twin();
        for (const auto& [game, tw] : focus.twins)
            if (tw.depth && tw.name == name && tw.gameDesc.SampleDesc.Count == 1)
                return tw.focus.Get();
        return d ? d->focus.Get() : nullptr;
    }
    for (const auto& [game, tw] : focus.twins)
        if (!tw.depth && tw.name == name && tw.gameDesc.SampleDesc.Count == 1)
            return tw.focus.Get();
    return nullptr;
}

// In our post list (open, our heap set), after the veil: per focus eye, ACE's VelocityAndDepthPacking replayed with
// focus constants into our four outputs, then our NGX DLSS feature for that eye over our colour twin, depth twin and
// packed motion vectors (each input the counterpart of what ACE's own evaluation named) into our DLSS image (slot 71).
// True when both eyes were evaluated (the tonemap then reads our DLSS image).
bool focus_dlss_record(List* l, const Stream& s, ID3D12Resource* colour, UINT fw2, UINT fh, UINT ow2, UINT oh,
                       const float (&centre)[2][2]) {
    if (focus.dlssBroken || !focus.packPso || !focus_ngx::installed())
        return false;
    const Stream::TileDispatch* pack[2] = {};
    for (const auto& d : s.tileDispatches)
        if (d.kind == 12) {
            if (!pack[0])
                pack[0] = &d;
            else if (!pack[1])
                pack[1] = &d;
        }
    focus_ngx::EyeEval game[2];
    static int missLogged = 0;
    const auto miss = [&](const char* why) {
        static int epoch = 0;
        if (miss_allowed(epoch, missLogged, 4)) {
            logd("replay focus: dlss: not this frame (%s)", why);
        }
        return false;
    };
    if (!pack[0] || !pack[1])
        return miss("ACE's two VelocityAndDepthPacking dispatches not in its post list");
    if (!focus_ngx::latest(0, game[0]) || !focus_ngx::latest(1, game[1]))
        return miss("ACE's DLSS evaluations of both eyes not seen yet");
    if (game[0].sequence <= focus.ngxUsed[0] || game[1].sequence <= focus.ngxUsed[1])
        return miss("ACE's DLSS evaluations not this frame's");
    const UINT fw = fw2 / 2;
    ID3D12Resource *depth = nullptr, *colourIn = nullptr, *motionIn = nullptr;
    const Twin* velocityTwin = nullptr;
    const Twin* flagsTwin = nullptr;
    UINT ew = 0, eh = 0;
    std::string why;
    AcquireSRWLockExclusive(&focus.lock);
    for (const auto& [g, tw] : focus.twins) {
        if (tw.depth || tw.gameDesc.SampleDesc.Count != 1)
            continue;
        if (tw.name == "velocity")
            velocityTwin = &tw;
        else if (tw.name == "flags")
            flagsTwin = &tw;
        else if (tw.name == "msaaColor") {
            ew = tw.eyeW;
            eh = tw.eyeH;
        }
    }
    if (!ew && focus.resolvedColour) {  // MSAA: the eye size from our resolved colour's stand-in
        ew = focus.resolvedColourTwin.eyeW;
        eh = focus.resolvedColourTwin.eyeH;
    }
    depth = focus.depthForTiles;  // the scene depth twin (slot 12's view)
    if (!velocityTwin && focus.resolvedVelocity && focus.resolveReady)  // MSAA: our resolve of the velocity twin
        velocityTwin = &focus.resolvedVelocityTwin;
    if (!velocityTwin || !flagsTwin || !depth || !ew || !focus.fogUavFirst || !focus.uavBaseIndex) {
        why = "the velocity / flags / depth twins or our UAV block (fog) not there";
    } else if (focus.packW != fw2 || focus.packH != fh || focus.packOutW != ow2 || focus.packOutH != oh ||
               focus.packColour != colour ||
               focus.packVelocityTwin != velocityTwin->focus.Get() || focus.packFlagsTwin != flagsTwin->focus.Get()) {
        for (ID3D12Resource* old : {focus.packAa.Get(), focus.packMb.Get(), focus.packVelocity.Get(), focus.packFlags.Get(),
                                    focus.dlssOut.Get(), focus.dlssOutAuto.Get(), focus.casOut.Get()})
            retire(old);
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, fw2, fh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        const auto make = [&](DXGI_FORMAT f, ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES state) {
            d.Format = f;
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
        };
        // ACE's DLSS output format (post_process_color) for our DLSS image
        const DXGI_FORMAT outFormat = game[0].output ? game[0].output->GetDesc().Format : DXGI_FORMAT_R11G11B10_FLOAT;
        const bool ok0 = make(DXGI_FORMAT_R16G16B16A16_FLOAT, focus.packAa, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                        make(DXGI_FORMAT_R16G16B16A16_FLOAT, focus.packMb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                        make(DXGI_FORMAT_R16G16_FLOAT, focus.packVelocity, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                        make(DXGI_FORMAT_R16_FLOAT, focus.packFlags, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        d.Width = ow2;  // our DLSS image and its sharpened copy: the output size
        d.Height = oh;
        const bool ok = ok0 && make(outFormat, focus.dlssOut, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) &&
                        make(outFormat, focus.dlssOutAuto, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) &&
                        make(outFormat, focus.casOut, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        if (!ok) {
            log_line("replay focus: dlss: could not make our packing outputs / DLSS image; no focus DLSS");
            focus.dlssBroken = true;
        } else {
            inReplay = true;
            const auto uav = [&](ID3D12Resource* r, UINT offset) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
                u.Format = r->GetDesc().Format;
                u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavViewOriginal(device.Get(), r, nullptr, &u, game_cpu(focus.fogUavFirst + offset));
            };
            uav(focus.packMb.Get(), 24);
            uav(focus.packVelocity.Get(), 25);
            uav(focus.packFlags.Get(), 26);
            uav(focus.packAa.Get(), 27);
            const auto srv = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
                D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                v.Format = f;
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                v.Texture2D.MipLevels = 1;
                srvViewOriginal(device.Get(), r, &v, game_cpu(focus.slotFirst + slot));
            };
            srv(velocityTwin->focus.Get(), velocityTwin->viewFormat, 69);
            srv(flagsTwin->focus.Get(), flagsTwin->viewFormat, 70);
            srv(focus.dlssOut.Get(), outFormat, 71);
            srv(focus.dlssOutAuto.Get(), outFormat, 72);
            srv(focus.casOut.Get(), outFormat, 73);
            uav(focus.casOut.Get(), 28);
            inReplay = false;
            focus.packW = fw2;
            focus.packOutW = ow2;
            focus.packOutH = oh;
            focus.packH = fh;
            focus.packColour = colour;
            focus.packVelocityTwin = velocityTwin->focus.Get();
            focus.packFlagsTwin = flagsTwin->focus.Get();
            logd("replay focus: dlss: our packing outputs and DLSS image %ux%u (output format %d); velocity twin format %d, "
                 "flags %d",
                 fw2, fh, static_cast<int>(outFormat), static_cast<int>(velocityTwin->viewFormat),
                 static_cast<int>(flagsTwin->viewFormat));
        }
    }
    // ACE's inputs -> ours, by name: the colour and depth twins, our packing outputs for the motion vectors.
    if (why.empty() && !focus.dlssBroken) {
        colourIn = (game[0].colorName == "msaaColor" || game[0].colorName == "upscaling") ? colour
                                                                                            : twin_named(game[0].colorName, false);
        const std::string& mv = game[0].motionName;
        motionIn = mv == "velocityAndDepthAA" ? focus.packAa.Get() : mv == "velocity" ? focus.packVelocity.Get() : nullptr;
        ID3D12Resource* depthNamed = !game[0].depth                                           ? nullptr
                                     : game[0].depthName == "closestDepth" && focus.closestDepth ? focus.closestDepth.Get()
                                                                                                  : twin_named(game[0].depthName, true);
        if (depthNamed)
            depth = depthNamed;
        if (!colourIn || !motionIn)
            why = "ACE's DLSS colour '" + game[0].colorName + "' or motion '" + mv + "' has no focus counterpart";
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (focus.dlssBroken)
        return false;
    if (!why.empty())
        return miss(why.c_str());
    if (pack[0]->tables[2].ptr != game_gpu(focus.uavBaseIndex).ptr)
        return miss("ACE's packing UAV table isn't the bindless UAV range our outputs are in");
    // Our NGX features at the focus eye size (made again on a size change: the old ones are left to NGX's shutdown).
    if (!focus.ngx[0] || !focus.ngx[1] || focus.ngxW != fw || focus.ngxH != fh || focus.ngxOutW != ow2 / 2 || focus.ngxOutH != oh ||
        focus.ngxPreset != focus_ngx::focus_preset() ||  // or the preset changed (the headset menu)
        !focus_ngx::alive(focus.ngx[0]) || !focus_ngx::alive(focus.ngx[1])) {  // or the game shut NGX down since
        focus.ngxPreset = focus_ngx::focus_preset();
        for (int e = 0; e < 2; ++e) {  // the ones before (another size or preset) are released once the GPU is done
            retire_feature(focus.ngx[e]);
            retire_feature(focus.ngxAuto[e]);
        }
        focus.ngx[0] = focus_ngx::make(l, fw, fh, 0, ow2 / 2, oh);
        focus.ngx[1] = focus.ngx[0] ? focus_ngx::make(l, fw, fh, 0, ow2 / 2, oh) : nullptr;
        focus.ngxOutW = ow2 / 2;
        focus.ngxOutH = oh;
        if (focus.settings.dlss == 3 && focus.ngx[1]) {  // A/B: a pair with DLSS's own auto-exposure (flag 0x40)
            focus.ngxAuto[0] = focus_ngx::make(l, fw, fh, 0x40, ow2 / 2, oh);
            focus.ngxAuto[1] = focus.ngxAuto[0] ? focus_ngx::make(l, fw, fh, 0x40, ow2 / 2, oh) : nullptr;
        }
        focus.ngxW = fw;
        focus.ngxH = fh;
        focus.dlssFresh = true;
        if (!focus.ngx[0] || !focus.ngx[1]) {
            log_line("replay focus: dlss: NGX refused our focus features; no focus DLSS");
            focus.dlssBroken = true;
            return false;
        }
        ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};  // NGX may set its own heaps
        l->SetDescriptorHeaps(1, heaps);
    }
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    const float k = static_cast<float>(fw) / (static_cast<float>(ew) * box);  // focus pixels per eye pixel (eyeW: per eye)
    const float ky = static_cast<float>(focus.sizeKy / std::max(1e-6f, boxY));  // and vertically
    const UINT eyeW = ew;
    // 1. The packing, per eye: its constants patched, then ACE's dispatch over our focus eye.
    barrier(l, focus.packVelocityTwin, rest_of(focus.packVelocityTwin, D3D12_RESOURCE_STATE_RENDER_TARGET), read);
    barrier(l, focus.packFlagsTwin, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    barrier(l, depth, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE), read);
    D3D12_GPU_VIRTUAL_ADDRESS cb[2];
    for (int e = 0; e < 2; ++e) {
        cb[e] = focus.packConstants->GetGPUVirtualAddress() + UINT64{focus.packSlot++ % kPostSlots} * 512;
        uint32_t p[18] = {};
        p[16] = boxYBits;
        std::memcpy(p + 17, &ky, 4);
        const float f[7] = {box, -centre[e][0], -centre[e][1], k, static_cast<float>(fw), static_cast<float>(fh),
                            static_cast<float>(e * fw)};
        std::memcpy(p, f, sizeof(f));
        const float before[2] = {-(centre[e][0] - focus.gazeOffset[e][0] + focus.gazePrev[e][0]),
                                 -(centre[e][1] - focus.gazeOffset[e][1] + focus.gazePrev[e][1])};
        std::memcpy(p + 14, before, sizeof(before));
        const UINT rel = focus.slotFirst - focus.baseIndex, urel = focus.fogUavFirst - focus.uavBaseIndex;
        p[7] = rel + 69;   // velocity
        p[8] = rel + 12;   // depth
        p[9] = rel + 70;   // flags
        p[10] = urel + 24; // MB
        p[11] = urel + 25; // velocity only
        p[12] = urel + 26; // flags output
        p[13] = urel + 27; // AA
        l->SetComputeRootSignature(focus.packRoot.Get());
        l->SetPipelineState(focus.packPso.Get());
        l->SetComputeRootConstantBufferView(0, pack[e]->cbv);
        l->SetComputeRoot32BitConstants(1, 18, p, 0);  // all of them: 14-17 are last frame's box and the box height
        l->SetComputeRootUnorderedAccessView(2, cb[e]);
        l->Dispatch(1, 1, 1);
    }
    barrier(l, focus.packConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    for (int e = 0; e < 2; ++e) {
        const Stream::TileDispatch& d = *pack[e];
        l->SetComputeRootSignature(d.root);
        l->SetPipelineState(d.pso);
        l->SetComputeRootConstantBufferView(0, cb[e]);
        for (int t = 1; t <= 4; ++t)
            l->SetComputeRootDescriptorTable(t, d.tables[t]);
        l->Dispatch((fw + 7) / 8, (fh + 7) / 8, 1);
    }
    barrier(l, focus.packConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(l, focus.packVelocityTwin, read, rest_of(focus.packVelocityTwin, D3D12_RESOURCE_STATE_RENDER_TARGET));
    barrier(l, focus.packFlagsTwin, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // 2. Our DLSS, per eye (its own feature and history).
    for (ID3D12Resource* r : {focus.packAa.Get(), focus.packVelocity.Get()})
        barrier(l, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(l, colour, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    barrier(l, focus.dlssOut.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool autoPair = focus.ngxAuto[0] && focus.ngxAuto[1];
    if (autoPair)
        barrier(l, focus.dlssOutAuto.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bool ok = true;
    for (int e = 0; e < 2; ++e) {
        focus_ngx::Inputs in;
        in.color = colourIn;
        in.depth = depth;
        in.motion = motionIn;
        in.output = focus.dlssOut.Get();
        in.exposure = game[e].exposure;  // ACE's own (its 1x1 luminance or none)
        in.base[0] = e * fw;
        in.base[1] = 0;
        in.outBase[0] = e * (ow2 / 2);  // the output's half of our DLSS image
        in.outBase[1] = 0;
        in.ownOutBase = true;
        in.width = fw;
        in.height = fh;
        // the focus views jitter by the same pixels as the eyes (kPatchShader); x as really drawn (focus_ngx)
        in.jitter[0] = game[e].jitter[0] * focus_ngx::jitter_x();
        in.jitter[1] = game[e].jitter[1];
        for (int a = 0; a < 2; ++a) {  // a scale that is a size (|s| > 4): the focus size's; else as ACE's
            const float sz = a == 0 ? static_cast<float>(fw) / static_cast<float>(eyeW)
                                    : static_cast<float>(fh) / static_cast<float>(eh);
            in.motionScale[a] = std::fabs(game[e].motionScale[a]) > 4 ? game[e].motionScale[a] * sz : game[e].motionScale[a];
        }
        in.game = &game[e];
        in.reset = focus.dlssFresh || game[e].reset != 0;
        ok = focus_ngx::evaluate(l, focus.ngx[e], in) && ok;
        if (autoPair) {
            in.output = focus.dlssOutAuto.Get();
            in.exposure = nullptr;
            focus_ngx::evaluate(l, focus.ngxAuto[e], in);
        }
        focus.ngxUsed[e] = game[e].sequence;
    }
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};  // NGX sets its own heaps
    l->SetDescriptorHeaps(1, heaps);
    barrier(l, focus.dlssOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (autoPair)
        barrier(l, focus.dlssOutAuto.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    barrier(l, colour, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(l, depth, read, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE));
    for (ID3D12Resource* r : {focus.packAa.Get(), focus.packVelocity.Get()})
        barrier(l, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!ok)
        return false;
    // 3. ACE's CAS (FidelityFX contrast-adaptive sharpening, its only constant the sharpness: size-independent) over our
    // DLSS image into casOut, as ACE sharpens its eyes' DLSS output.
    focus.casDone = false;
    for (const auto& d : s.tileDispatches) {
        if (d.kind != 13)
            continue;
        barrier(l, focus.casOut.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetComputeRootSignature(d.root);
        l->SetPipelineState(d.pso);
        l->SetComputeRoot32BitConstants(0, 1, d.constants, 0);
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 71));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.fogUavFirst + 28));
        l->Dispatch((ow2 + 15) / 16, (oh + 15) / 16, 1);
        barrier(l, focus.casOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        focus.casDone = true;
        static bool casLogged = false;
        if (!casLogged) {
            casLogged = true;
            float sharp;
            std::memcpy(&sharp, d.constants, 4);
            logd("replay focus: dlss: ACE's CAS replayed on our DLSS image (its constant 0x%08x / %.4g)", d.constants[0], sharp);
        }
        break;
    }
    if (!focus.dlssReady)
        logd("replay focus: dlss: the first focus DLSS recorded (colour %s, depth %s, motion %s -> ours; jitter as ACE's (%.2f focus pixels per eye pixel), "
             "motion scale %.4g,%.4g -> ours)",
             game[0].colorName.c_str(), game[0].depthName.c_str(), game[0].motionName.c_str(), k, game[0].motionScale[0],
             game[0].motionScale[1]);
    focus.dlssReady = true;
    focus_ngx::note_focus_running(true);
    focus.dlssFresh = false;
    focus.dlssEvaluations.fetch_add(2, std::memory_order_relaxed);
    return true;
}

// ---- P2c: the focus views' MSAA antialiasing (DLSS off) -------------------------------------------------------------

// In our post list (open, our heap set): ACE's vrAntialiasing (one dispatch per eye in its post list: per-view constants
// at root 6; tables t0 colour MS, t1 unused, t2 depthResolveTarget, t3 prev_frame_color, u0 post_process_color, u1
// unused) replayed per focus eye with its constants patched, from our multisampled colour twin, our scene depth and
// our history into aaOut, which is then copied into our history. True when done (the tonemap reads aaOut, slot 78).
bool focus_aa_record(List* l, const Stream& s, UINT fw2, UINT fh, const float (&centre)[2][2]) {
    if (focus.aaBroken || !focus.aaPatchPso || !focus.fogUavFirst)
        return false;
    const Stream::TileDispatch* aa[2] = {};
    for (const auto& d : s.tileDispatches)
        if (d.kind == 16 && d.cbv6) {
            if (!aa[0])
                aa[0] = &d;
            else if (!aa[1])
                aa[1] = &d;
        }
    if (!aa[0] || !aa[1]) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::string names;
            for (ID3D12PipelineState* pso : s.dispatchPsos) {
                wchar_t n[64] = {};
                UINT size = sizeof(n) - sizeof(wchar_t);
                if (pso && SUCCEEDED(pso->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, n))) {
                    names += ' ';
                    for (const wchar_t* c = n; *c; ++c)
                        names += static_cast<char>(*c);
                } else {
                    names += " ?";
                }
            }
            size_t k16 = 0, cb = 0;
            for (const auto& d : s.tileDispatches)
                if (d.kind == 16) {
                    ++k16;
                    cb += d.cbv6 != 0;
                }
            logf("replay focus: msaa: ACE's antialiasing not found as two dispatches in its post list (kind-16 dispatches %zu, "
                 "with a root-6 CBV %zu); the list's compute dispatches:%s",
                 k16, cb, names.c_str());
        }
        return false;
    }
    ID3D12Resource* ms = focus.resolvedColourTwin.eye.Get();  // the multisampled colour twin (see focus_post_record)
    ID3D12Resource* colour = focus.resolvedColour.Get();
    if (!ms || !colour)
        return false;
    const UINT fw = fw2 / 2;
    AcquireSRWLockExclusive(&focus.lock);
    if (focus.aaSource != ms || !focus.aaOut || focus.aaOut->GetDesc().Width != fw2) {
        retire(focus.aaOut.Get());
        retire(focus.aaHistory.Get());
        focus.aaOut.Reset();
        focus.aaHistory.Reset();
        D3D12_RESOURCE_DESC d = colour->GetDesc();
        d.Alignment = 0;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, read, nullptr, IID_PPV_ARGS(&focus.aaOut))) ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, read, nullptr,
                                                   IID_PPV_ARGS(&focus.aaHistory)))) {
            log_line("replay focus: msaa: could not make our antialiasing targets; no focus MSAA resolve");
            focus.aaBroken = true;
        } else {
            inReplay = true;
            const DXGI_FORMAT f = focus.resolvedColourTwin.viewFormat;
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = f;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            srvViewOriginal(device.Get(), ms, &v, game_cpu(focus.slotFirst + 76));
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            srvViewOriginal(device.Get(), focus.aaHistory.Get(), &v, game_cpu(focus.slotFirst + 77));
            srvViewOriginal(device.Get(), focus.aaOut.Get(), &v, game_cpu(focus.slotFirst + 78));
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = f;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavViewOriginal(device.Get(), focus.aaOut.Get(), nullptr, &u, game_cpu(focus.fogUavFirst + 32));
            inReplay = false;
            focus.aaSource = ms;
            focus.aaFresh = true;
            logd("replay focus: msaa: our antialiased focus colour and history %ux%u (ACE's vrAntialiasing replayed)", fw2, fh);
        }
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (focus.aaBroken || !focus.aaOut)
        return false;
    // 1. Per eye: the constants patched.
    D3D12_GPU_VIRTUAL_ADDRESS cb[2];
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    l->SetComputeRootSignature(focus.aaPatchRoot.Get());
    l->SetPipelineState(focus.aaPatchPso.Get());
    for (int e = 0; e < 2; ++e) {
        cb[e] = focus.aaConstants->GetGPUVirtualAddress() + UINT64{focus.aaSlot++ % kPostSlots} * 512;
        const float p[9] = {box, -centre[e][0], -centre[e][1], static_cast<float>(fw), static_cast<float>(fh),
                            static_cast<float>(e * fw), -(centre[e][0] - focus.gazeOffset[e][0] + focus.gazePrev[e][0]),
                            -(centre[e][1] - focus.gazeOffset[e][1] + focus.gazePrev[e][1]), boxY};
        l->SetComputeRootConstantBufferView(0, aa[e]->cbv6);
        l->SetComputeRoot32BitConstants(1, 9, p, 0);
        l->SetComputeRootUnorderedAccessView(2, cb[e]);
        l->Dispatch(1, 1, 1);
    }
    barrier(l, focus.aaConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    // 2. ACE's pipeline per eye. A fresh history (first frame) starts from the current resolve.
    ID3D12Resource* depth = focus.depthForTiles;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (focus.aaFresh) {
        barrier(l, colour, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(l, focus.aaHistory.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(focus.aaHistory.Get(), colour);
        barrier(l, focus.aaHistory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        barrier(l, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        focus.aaFresh = false;
    }
    barrier(l, ms, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(l, depth, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE), read);
    barrier(l, focus.aaOut.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (int e = 0; e < 2; ++e) {
        const Stream::TileDispatch& d = *aa[e];
        l->SetComputeRootSignature(d.root);
        l->SetPipelineState(d.pso);
        l->SetComputeRootDescriptorTable(0, game_gpu(focus.slotFirst + 76));
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 77));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 12));
        l->SetComputeRootDescriptorTable(3, game_gpu(focus.slotFirst + 77));
        l->SetComputeRootDescriptorTable(4, game_gpu(focus.fogUavFirst + 32));
        l->SetComputeRootDescriptorTable(5, game_gpu(focus.fogUavFirst + 32));
        l->SetComputeRootConstantBufferView(6, cb[e]);
        l->Dispatch((fw + 7) / 8, (fh + 7) / 8, 1);
    }
    barrier(l, focus.aaOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(l, depth, read, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE));
    barrier(l, ms, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(l, focus.aaConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // 3. Our history (ACE copies post_process_color into prev_frame_color the same way).
    barrier(l, focus.aaHistory.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(focus.aaHistory.Get(), focus.aaOut.Get());
    barrier(l, focus.aaHistory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    barrier(l, focus.aaOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (!focus.aaReady)
        logd("replay focus: msaa: the first focus antialiasing recorded (ACE's vrAntialiasing per eye)");
    focus.aaReady = true;
    focus.aaReplays.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- P1: the focus views' post: bloom, tonemap, fxaa -------------------------------------------------------------

// The game's view (srvInfo) a descriptor table points at: its resource and view, or false.
bool table_view(D3D12_GPU_DESCRIPTOR_HANDLE table, ComPtr<ID3D12Resource>& r, D3D12_SHADER_RESOURCE_VIEW_DESC& v) {
    const UINT64 heapGpu = focus.gameHeap->GetGPUDescriptorHandleForHeapStart().ptr;
    if (table.ptr < heapGpu)
        return false;
    const auto it = focus.srvInfo.find(game_cpu(static_cast<UINT>((table.ptr - heapGpu) / focus.increment)).ptr);
    if (it == focus.srvInfo.end())
        return false;
    r = it->second.first;
    v = it->second.second;
    return true;
}
}  // namespace

// At the game's Close of its post list (tonemap and fxaa drawn): per frame, the eye's veil (bloom) resampled through
// each focus box into our veil; the game's tonemap replayed into our focus image (our colour and veil; the game's
// exposure, grading LUT; its constants with our size); its fxaa into our final focus image. Our slots: 56 our colour
// twin's SRV, 57 the game veil's SRV, 58/59 our veil UAV/SRV, 60 our tonemapped image's SRV.
void focus_post_record(List* gameList, const Stream& s) {
    Replay* r = nullptr;
    UINT fw2 = 0, fh = 0, ow2 = 0, oh = 0;  // the focus views' render size and their output size (our DLSS upscaling)
    ID3D12Resource* colour = nullptr;
    float centre[2][2] = {};
    std::string why;
    AcquireSRWLockExclusive(&focus.lock);
    const Twin* twin = nullptr;
    const Twin* colourMs = nullptr;  // MSAA: the multisampled colour twin, resolved into ours first
    for (const auto& [game, tw] : focus.twins) {
        if (!tw.depth && tw.name == "msaaColor" && tw.gameDesc.SampleDesc.Count == 1)
            twin = &tw;
        else if (!tw.depth && tw.name == "msaaColor")
            colourMs = &tw;
    }
    if (!twin && colourMs) {
        if (!focus.resolvedColour || focus.resolvedColourTwin.eye.Get() != colourMs->focus.Get()) {
            retire(focus.resolvedColour.Get());
            focus.resolvedColour.Reset();
            D3D12_RESOURCE_DESC d = colourMs->focus->GetDesc();
            d.SampleDesc = {1, 0};
            d.Alignment = 0;  // the multisampled twin's 4 MB placement alignment is refused for one sample
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                          nullptr, IID_PPV_ARGS(&focus.resolvedColour)))) {
                focus.resolvedColourTwin = *colourMs;
                focus.resolvedColourTwin.focus = focus.resolvedColour;
                focus.resolvedColourTwin.eye = colourMs->focus;  // here: the multisampled source
                focus.resolvedColourTwin.gameDesc.SampleDesc = {1, 0};
                logd("replay focus: msaa: our resolved focus colour %llux%u (from the %u-sample twin)",
                     static_cast<unsigned long long>(d.Width), d.Height, colourMs->gameDesc.SampleDesc.Count);
            } else {
                static bool failLogged = false;
                if (!failLogged) {
                    failLogged = true;
                    log_line("replay focus: msaa: could not make our resolved focus colour");
                }
            }
        }
        if (focus.resolvedColour)
            twin = &focus.resolvedColourTwin;
    }
    if (focus.postBroken || focus.tilesBroken || !focus.gameHeap || !focus.slotFirst || !focus.cbPatchPso ||
        !focus.atmoPso[0] || !focus.centresKnown || !twin) {
        why = "not ready (tiles, centres or the colour twin)";
    } else {
        fw2 = 2 * twin->focusW;
        fh = twin->focusH;
        // Our DLSS upscaling: the post works on its output, the focus views' full size.
        ow2 = fw2;
        oh = fh;
        if (focus.settings.dlss && focus.renderScale < 0.999) {
            ow2 = 2 * std::max(16u, static_cast<UINT>(std::lround(twin->focusW / focus.renderScale)));
            oh = std::max(16u, static_cast<UINT>(std::lround(twin->focusH / focus.renderScale)));
        }
        colour = twin->focus.Get();
        focus_centres(centre);
        if (!focus.postLdr || ow2 != focus.postW || oh != focus.postH || focus.postColour != colour ||
            focus.postFxaa != s.hasFxaa) {
            ComPtr<ID3D12Resource> veil, fxaaTex;
            D3D12_SHADER_RESOURCE_VIEW_DESC veilView{}, fxaaView{};
            // Without ACE's fxaa draw (its menus; frame-600 17:19 capture) the tonemap's target is the final image: our
            // targets take its format, and the tonemap is replayed straight into our final focus image.
            const auto tonemapRt = focus.views.find(s.tonemapDraw.rtv);
            const auto fxaaRt = s.hasFxaa ? focus.views.find(s.fxaaDraw.rtv) : tonemapRt;
            bool fxaaSeen = tonemapRt != focus.views.end() && fxaaRt != focus.views.end();
            if (fxaaSeen && s.hasFxaa) {
                fxaaSeen = table_view(s.fxaaDraw.tables[1], fxaaTex, fxaaView);
            } else if (fxaaSeen) {
                fxaaTex = tonemapRt->second.resource;
                fxaaView.Format = tonemapRt->second.viewFormat;
                fxaaView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                fxaaView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                fxaaView.Texture2D.MipLevels = 1;
                fxaaSeen = fxaaTex != nullptr;
            }
            if (!fxaaSeen || !table_view(s.tonemapDraw.tables[3], veil, veilView)) {
                why = "the game's veil / fxaa_texture views or render targets not seen";
            } else {
                for (ID3D12Resource* old : {focus.postLdr.Get(), focus.postFinal.Get(), focus.postVeil.Get()})
                    retire(old);
                if (focus.postLdrRtvSlot < 0)
                    focus.postLdrRtv = view_slot(false, focus.postLdrRtvSlot);
                if (focus.postFinalRtvSlot < 0)
                    focus.postFinalRtv = view_slot(false, focus.postFinalRtvSlot);
                D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
                D3D12_RESOURCE_DESC vd = veil->GetDesc();
                vd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                vd.MipLevels = 1;
                D3D12_RESOURCE_DESC ld = fxaaTex->GetDesc();
                ld.Width = ow2;
                ld.Height = oh;
                ld.MipLevels = 1;
                ld.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_RESOURCE_DESC fd = ld;
                fd.Format = fxaaRt->second.viewFormat;
                // ACE's fxaa_texture is typed UNORM with _SRGB views (format casting set up by the game): ours are
                // typeless, which allows both.
                const auto typeless = [](DXGI_FORMAT f) {
                    switch (f) {
                    case DXGI_FORMAT_R8G8B8A8_UNORM:
                    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
                    case DXGI_FORMAT_B8G8R8A8_UNORM:
                    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
                    default:
                        return f;
                    }
                };
                ld.Format = typeless(ld.Format);
                fd.Format = typeless(fd.Format);
                bool ok = focus.postLdrRtv.ptr && focus.postFinalRtv.ptr;
                ComPtr<ID3D12Resource> v2, l2, f2;
                ok = ok &&
                     SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &vd,
                                                               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                               IID_PPV_ARGS(&v2))) &&
                     SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ld,
                                                               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                               IID_PPV_ARGS(&l2))) &&
                     SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &fd,
                                                               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                               IID_PPV_ARGS(&f2)));
                if (!ok) {
                    log_line("replay focus: post: could not make our post targets; no focus post");
                    focus.postBroken = true;
                } else {
                    inReplay = true;
                    D3D12_SHADER_RESOURCE_VIEW_DESC cv{};
                    cv.Format = twin->viewFormat;
                    cv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    cv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    cv.Texture2D.MipLevels = 1;
                    srvViewOriginal(device.Get(), colour, &cv, game_cpu(focus.slotFirst + 56));
                    srvViewOriginal(device.Get(), veil.Get(), &veilView, game_cpu(focus.slotFirst + 57));
                    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
                    u.Format = vd.Format;
                    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    uavViewOriginal(device.Get(), v2.Get(), nullptr, &u, game_cpu(focus.slotFirst + 58));
                    D3D12_SHADER_RESOURCE_VIEW_DESC ov = veilView;
                    ov.Texture2D.MostDetailedMip = 0;
                    ov.Texture2D.MipLevels = 1;
                    srvViewOriginal(device.Get(), v2.Get(), &ov, game_cpu(focus.slotFirst + 59));
                    D3D12_SHADER_RESOURCE_VIEW_DESC lv = fxaaView;
                    lv.Texture2D.MostDetailedMip = 0;
                    lv.Texture2D.MipLevels = 1;
                    srvViewOriginal(device.Get(), l2.Get(), &lv, game_cpu(focus.slotFirst + 60));
                    D3D12_RENDER_TARGET_VIEW_DESC rv{};
                    rv.Format = tonemapRt->second.viewFormat;
                    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    rtvOriginal(device.Get(), l2.Get(), &rv, focus.postLdrRtv);
                    rv.Format = fxaaRt->second.viewFormat;
                    rtvOriginal(device.Get(), f2.Get(), &rv, focus.postFinalRtv);
                    inReplay = false;
                    focus.postVeil = v2;
                    focus.postLdr = l2;
                    focus.postLdrRtvFormat = rv.Format == fxaaRt->second.viewFormat ? tonemapRt->second.viewFormat : rv.Format;
                    for (ComPtr<ID3D12Resource>& a : focus.postAlt) {
                        retire(a.Get());
                        a.Reset();
                    }
                    focus.postFinal = f2;
                    focus.postW = ow2;
                    focus.postH = oh;
                    focus.postColour = colour;
                    focus.postFxaa = s.hasFxaa;
                    focus.postGameFxaa = s.hasFxaa ? fxaaTex : nullptr;  // the samples' comparison: only ACE's fxaa_texture
                    focus.postGameFxaaFormat = fxaaView.Format;
                    if (!s.hasFxaa)
                        log_line("replay focus: post: ACE's post has no fxaa draw (its menus): the tonemap goes straight "
                                 "into our final focus image");
                    logd("replay focus: post: our focus post targets %ux%u (tonemap format %d, final %d), veil %llux%u",
                         ow2, oh, static_cast<int>(rv.Format == fxaaRt->second.viewFormat ? tonemapRt->second.viewFormat
                                                                                          : rv.Format),
                         static_cast<int>(fxaaRt->second.viewFormat), static_cast<unsigned long long>(vd.Width), vd.Height);
                }
            }
        }
        if (focus.postLdr && !focus.postBroken && focus.settings.dlss >= 2 && !focus.postAlt[0]) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            const D3D12_RESOURCE_DESC ld = focus.postLdr->GetDesc();
            inReplay = true;
            for (int a = 0; a < 2; ++a) {
                if (focus.postAltRtvSlot[a] < 0)
                    focus.postAltRtv[a] = view_slot(false, focus.postAltRtvSlot[a]);
                if (!focus.postAltRtv[a].ptr ||
                    FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ld,
                                                           D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                           IID_PPV_ARGS(&focus.postAlt[a])))) {
                    focus.postAlt[a].Reset();
                    continue;
                }
                D3D12_RENDER_TARGET_VIEW_DESC rv{};
                rv.Format = focus.postLdrRtvFormat;
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtvOriginal(device.Get(), focus.postAlt[a].Get(), &rv, focus.postAltRtv[a]);
            }
            inReplay = false;
            if (focus.postAlt[0] && focus.postAlt[1])
                log_line("replay focus: dlss A/B: the same frame's tonemap also from our colour and from DLSS without CAS");
        }
        if (focus.postLdr && !focus.postBroken)
            r = acquire_replay();
    }
    static int missLogged = 0;
    static int missEpoch = 0;
    if (!r && !why.empty() && focus.twins.size() > 4 && miss_allowed(missEpoch, missLogged, 3)) {
        logd("replay focus: post: not replayed (%s)", why.c_str());
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepPostVeil);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    // 0. MSAA: our multisampled colour twin resolved into our single-sample colour (as ACE resolves msaaColor into
    // upscaling before its DLSS).
    if (colour == focus.resolvedColour.Get())
        resolve_focus_colour(l);
    // 1. The veil (bloom) through each focus box, both halves.
    const D3D12_RESOURCE_DESC vd = focus.postVeil->GetDesc();
    {
        const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
        uint32_t p[10] = {};
        const float f0[3] = {box, centre[0][0], centre[0][1]};
        std::memcpy(p, f0, sizeof(f0));
        p[3] = static_cast<uint32_t>(vd.Width);
        p[4] = vd.Height;
        p[5] = 1;
        p[6] = 1;  // stereo: two halves
        std::memcpy(&p[7], &centre[1][0], 4);
        std::memcpy(&p[8], &centre[1][1], 4);
        barrier(l, focus.postVeil.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetComputeRootSignature(focus.atmoRoot.Get());
        l->SetPipelineState(focus.atmoPso[0].Get());
        l->SetComputeRoot32BitConstants(0, 10, p, 0);
        l->SetComputeRoot32BitConstants(0, 1, &boxYBits, 10);  // the box's height (kAtmoShader boxY)
        atmo_source_region(l, true, true);  // ACE's veil: after its DLSS
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 57));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 58));
        l->Dispatch((p[3] + 7) / 8, (p[4] + 7) / 8, 1);
        barrier(l, focus.postVeil.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    // 1b. P2a: our DLSS per focus eye (ACE's packing replayed for our motion vectors), read by the tonemap below.
    gpu_stamp(l, kStepPostDlss);
    // NGX's lifetime before our evaluations are recorded: a shutdown after this makes the stamp stale, never the reverse
    const uint64_t ngxLife = focus_ngx::lifetime();
    const bool dlssUsed = focus.settings.dlss && focus_dlss_record(l, s, colour, fw2, fh, ow2, oh, centre);
    // 1c. P2c: MSAA without DLSS: ACE's own antialiasing replayed (our colour twin multisampled, ACE's pass present)
    const bool aaUsed = !dlssUsed && colour == focus.resolvedColour.Get() && focus_aa_record(l, s, fw2, fh, centre);
    gpu_stamp(l, kStepPostTone);
    // 2. The tonemap constants with our size (register 1: height, width, 1/height, 1/width).
    const D3D12_GPU_VIRTUAL_ADDRESS mine = focus.postConstants->GetGPUVirtualAddress() + UINT64{focus.postSlot++ % kPostSlots} * 256;
    {
        uint32_t p[8] = {6, 1, 0, 0};
        const float size[4] = {static_cast<float>(oh), static_cast<float>(ow2), 1.0f / static_cast<float>(oh),
                               1.0f / static_cast<float>(ow2)};
        std::memcpy(&p[4], size, sizeof(size));
        l->SetComputeRootSignature(focus.cbPatchRoot.Get());
        l->SetPipelineState(focus.cbPatchPso.Get());
        l->SetComputeRootConstantBufferView(0, s.tonemapDraw.cbv);
        l->SetComputeRoot32BitConstants(1, 8, p, 0);
        l->SetComputeRootUnorderedAccessView(2, mine);
        l->Dispatch(1, 1, 1);
        barrier(l, focus.postConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_VIEWPORT view{0, 0, static_cast<float>(ow2), static_cast<float>(oh), 0, 1};
    const D3D12_RECT rect{0, 0, static_cast<LONG>(ow2), static_cast<LONG>(oh)};
    // 3. The game's tonemap into our focus image.
    barrier(l, colour, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    ID3D12Resource* const tonemapTarget = s.hasFxaa ? focus.postLdr.Get() : focus.postFinal.Get();
    barrier(l, tonemapTarget, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    l->OMSetRenderTargets(1, s.hasFxaa ? &focus.postLdrRtv : &focus.postFinalRtv, FALSE, nullptr);
    l->RSSetViewports(1, &view);
    l->RSSetScissorRects(1, &rect);
    l->SetGraphicsRootSignature(s.tonemapDraw.root);
    l->SetPipelineState(s.tonemapDraw.pso);
    const UINT postSource = aaUsed ? 78 : !dlssUsed ? 56 : focus.casDone ? 73 : 71;
    l->SetGraphicsRootDescriptorTable(0, game_gpu(focus.slotFirst + postSource));
    l->SetGraphicsRootDescriptorTable(1, s.tonemapDraw.tables[1]);
    l->SetGraphicsRootDescriptorTable(2, s.tonemapDraw.tables[2]);
    l->SetGraphicsRootDescriptorTable(3, game_gpu(focus.slotFirst + 59));
    l->SetGraphicsRootDescriptorTable(4, s.tonemapDraw.tables[4]);
    l->SetGraphicsRootDescriptorTable(5, s.tonemapDraw.tables[5]);
    l->SetGraphicsRootConstantBufferView(6, mine);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->DrawInstanced(3, 1, 0, 0);
    barrier(l, tonemapTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    // replay_dlss=2: the same tonemap from our colour (no DLSS) and from DLSS without CAS (auto-exposure pair: 3)
    if (dlssUsed && focus.postAlt[0] && focus.postAlt[1]) {
        const UINT sources[2] = {56, focus.ngxAuto[0] ? 72u : 71u};
        for (int a = 0; a < 2; ++a) {
            barrier(l, focus.postAlt[a].Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            l->OMSetRenderTargets(1, &focus.postAltRtv[a], FALSE, nullptr);
            l->SetGraphicsRootDescriptorTable(0, game_gpu(focus.slotFirst + sources[a]));
            l->DrawInstanced(3, 1, 0, 0);
            barrier(l, focus.postAlt[a].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
    }
    barrier(l, colour, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // 4. The game's fxaa into our final focus image (rcpFrame ours); not in its menus (no fxaa draw: step 3 wrote it).
    if (s.hasFxaa) {
        barrier(l, focus.postFinal.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        l->OMSetRenderTargets(1, &focus.postFinalRtv, FALSE, nullptr);
        l->SetGraphicsRootSignature(s.fxaaDraw.root);
        l->SetPipelineState(s.fxaaDraw.pso);
        uint32_t consts[8];
        std::memcpy(consts, s.fxaaDraw.consts, sizeof(consts));
        const float rcp[2] = {1.0f / static_cast<float>(ow2), 1.0f / static_cast<float>(oh)};
        std::memcpy(consts, rcp, sizeof(rcp));
        l->SetGraphicsRoot32BitConstants(0, 8, consts, 0);
        l->SetGraphicsRootDescriptorTable(1, game_gpu(focus.slotFirst + 60));
        l->DrawInstanced(3, 1, 0, 0);
        barrier(l, focus.postFinal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    barrier(l, focus.postConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->Close();
    inReplay = false;
    r->ngxLife = focus.settings.dlss ? ngxLife : 0;  // (not dlssUsed: a refused second eye leaves the first's in the list)
    AcquireSRWLockExclusive(&focus.lock);
    add_pending(gameList, r);
    if (!focus.postReady)
        logd("replay focus: post: the first focus post replay recorded (bloom, tonemap, fxaa)");
    focus.postReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.postFrame.store(focus.frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    focus.postReplays.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace simqvfr::replay_detail
