// Part of mvr_pass.cpp's translation unit (included once, in order): the public API: install, the passes' begin and
// end, the options, the panel redraw. Included only by mvr_pass.cpp, inside namespace lmu, after its anonymous
// namespace closes, after mvr_pass_hook_install.h; never include it anywhere else: the per-draw path stays in one
// translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

bool mvr_pass_install(ID3D11Device* device) {
    if (g.context)
        return true;
    if (!device)
        return false;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    const D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const float identity[4] = {1, 0, 1, 0};
    const D3D11_SUBRESOURCE_DATA init = {identity, 0, 0};
    if (FAILED(device->CreateBuffer(&cd, &init, &g.remapCb)))
        return false;
    if (g.biasHooks) {  // texture sharpening's constants (no rectangle, no bias until a pass sets them)
        const D3D11_BUFFER_DESC bd = {sizeof(g.biasConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA zero = {g.biasConstants, 0, 0};
        if (FAILED(device->CreateBuffer(&bd, &zero, &g.biasCb)))
            g.biasHooks = false;
    }
    if (g.slotsOn) {  // car slots: the slot words and a view of each, the motions, the default blend state's twins
        std::vector<uint32_t> words(kCarSlots + 1);
        for (uint32_t i = 0; i <= kCarSlots; ++i)
            words[i] = i;  // slot s's view is element s + 1: its word s + 1 (0: none)
        const D3D11_BUFFER_DESC wd = {static_cast<UINT>(words.size() * 4), D3D11_USAGE_IMMUTABLE,
                                      D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA wi = {words.data(), 0, 0};
        const D3D11_BUFFER_DESC md = {kCarSlots * 48, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                      D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 48};
        std::vector<float> motionInit(size_t{kCarSlots} * 12, 0.0f);
        for (size_t i = 0; i < kCarSlots; ++i)
            motionInit[i * 12] = motionInit[i * 12 + 4] = motionInit[i * 12 + 8] = 1.0f;
        const D3D11_SUBRESOURCE_DATA mi = {motionInit.data(), 0, 0};
        bool ok = SUCCEEDED(device->CreateBuffer(&wd, &wi, &g.slotWords)) &&
                  SUCCEEDED(device->CreateBuffer(&md, &mi, &g.motionBuffer)) &&
                  SUCCEEDED(device->CreateShaderResourceView(g.motionBuffer.Get(), nullptr, &g.motionSrv));
        g.slotViews.resize(kCarSlots);
        for (UINT i = 0; ok && i < kCarSlots; ++i) {
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R32_UINT;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.FirstElement = i + 1;
            v.Buffer.NumElements = 1;
            ok = SUCCEEDED(device->CreateShaderResourceView(g.slotWords.Get(), &v, &g.slotViews[i]));
        }
        for (int w = 0; ok && w < 2; ++w) {
            const D3D11_BLEND_DESC plain = {FALSE, FALSE,
                                            {{FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                                              D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL}}};
            const D3D11_BLEND_DESC twin = blend_twin_desc(plain, w == 1);
            ok = SUCCEEDED(device->CreateBlendState(&twin, &g.defaultBlendTwin[w]));
        }
        ok = ok && SUCCEEDED(context.As(&g.context1));
        if (!ok) {
            g.slotsOn = false;
            g.slotViews.clear();
            simqvfr::log_line("lmu mvr: car slots: their buffers, views or blend states couldn't be made; off");
        }
    }
    const D3D11_QUERY_DESC dq = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, tq = {D3D11_QUERY_TIMESTAMP, 0};
    for (GpuFrame& f : g.gpu) {
        device->CreateQuery(&dq, &f.disjoint);
        for (auto& pass : f.stamp)
            for (auto& q : pass)
                device->CreateQuery(&tq, &q);
        for (auto& pass : f.corridor)
            for (auto& q : pass)
                device->CreateQuery(&tq, &q);
    }
    if (!simqvfr::minhook_ready(nullptr))  // no line here, as before
        return false;
    // The probe's way to the two views (tests set it with mvr_use_instanced): instancing hooks the draws too.
    if (mvr_probe_backend() == MvrBackend::instanced)
        g.instanced = true;
    if (mvr_probe_instanced())
        g.canInstance = true;
    g.context = context.Get();
    context.As(&g.multithread);
    {  // the game's VS b13 before its binds are seen
        ID3D11Buffer* b = nullptr;
        context->VSGetConstantBuffers(13, 1, &b);
        hold(g.gameVsCb13, b);
        if (b)
            b->Release();
    }
    hook_every_mode();
    for (int h = 0; h < kHooks; ++h)
        if (wanted(h) && !functions[h].hooked) {
            unhook_entries();
            g.context = nullptr;
            g.multithread.Reset();
            simqvfr::logf("lmu mvr: the context's %s couldn't be hooked; off", kHookName[h]);
            return false;
        }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g.qpcAtLog = now.QuadPart;
    g.tscAtLog = __rdtsc();
    std::string counts;
    for (int h = 0; h < kHooks; ++h)
        if (wanted(h)) {
            counts += (h ? ", " : "") + std::string(kHookName[h]) + " " + std::to_string(functions[h].hooked);
            ++g.stats.contextEntries;
        }
    simqvfr::logf("lmu mvr: on: passes 0-1 draw their focus views too (%s, beside the eye%s); the context's hooks on %llu "
                  "functions in d3d11's code (%s); multithread protection %s",
                  g.instanced ? mvr_backend_name(MvrBackend::instanced) : mvr_backend_name(MvrBackend::nvapi),
                  !g.instanced && g.canInstance ? "; instancing if the driver refuses it" : "",
                  static_cast<unsigned long long>(g.stats.contextFunctions), counts.c_str(),
                  !g.multithread ? "not known" : g.protectedNow ? "on" : "off");
    if (g.carTrack)
        simqvfr::logf("lmu mvr: car tracking on (log only): each eye-pass draw whose vertex shader reads a worldMatrix "
                      "keyed by its buffers and index range, its matrix read as the game writes the per-draw buffer");
    if (g.slotsOn)
        simqvfr::logf("lmu mvr: car slots on (level %d): the scene's binds get a slot target at render target 1, the "
                      "pixel shaders their slot twins, the blend states twins writing it where depth is written; %s",
                      g.slotsLevel, g.slotsLevel >= 2 ? "DLSS's vectors take the parts' motion" : "DLSS as before");
    return true;
}

void mvr_pass_begin(int pass, const MvrPassView* view, bool sample, bool replaceResolve) {
    if (!g.context)
        return;
    Timed timed(g.time.hooks);
    // A function an entry holds that wasn't seen at install (a mode of d3d11's not met there): hooked from this pass on.
    for (int h = 0; h < kHooks; ++h)
        if (hook_entry(h))
            simqvfr::logf("lmu mvr: the context's %s entry holds a function not seen before: hooked (%d for it now)",
                          kHookName[h], functions[h].count);
    if (pass == 0 && g.multithread) {  // who turns multithread protection on or off, and when (once per frame)
        const bool on = g.multithread->GetMultithreadProtected() != FALSE;
        if (on != g.protectedNow) {
            g.protectedNow = on;
            simqvfr::logf("lmu mvr: the immediate context's multithread protection went %s (game frame %llu)",
                          on ? "on" : "off", g.gpuFrameNo);
        }
    }
    g.pass = pass;
    if (pass == 0 && census.pre) {  // level 4: the census begun at the frame before goes on into this pass
        census.pre = false;
        census.active = true;
        simqvfr::logf("lmu mvr: pass census %llu: pass 0 begins after %d draws; eye %ux%u", census.taken, census.draws,
                      view ? view->eyeW : 0, view ? view->eyeH : 0);
    } else if (pass == 0 && census.level) {  // a census every kCensusEvery frames, and at the first pass 0 at a new eye size
        const unsigned w = view ? view->eyeW : 0, h = view ? view->eyeH : 0;
        const bool resized = w && (w != census.lastEye[0] || h != census.lastEye[1]);
        // (level 3, the whole pass: only at a new size, its reads stall the frame for a while)
        census.active = census.level >= 4 ? false
                        : census.level >= 3 ? resized && census.dumps < kCensusDumps
                                          : g.framesSinceInstall >= census.next || resized;
        if (census.active) {
            census.next = g.framesSinceInstall + kCensusEvery;
            census.lastEye[0] = w;
            census.lastEye[1] = h;
            census.draws = 0;
            ++census.taken;
            census.dumpThis = census.level == 2 && resized && census.dumps < kCensusDumps;
            if (census.level >= 3 || census.dumpThis)
                ++census.dumps;
            censusSaved.clear();
            simqvfr::logf("lmu mvr: post census %llu: frame %llu, pass 0: eye %ux%u (its final %ux%u), focus %ux%u%s; every "
                          "draw from the resolve (or the post's start) to the pass's end follows%s",
                          census.taken, g.framesSinceInstall, w, h, view ? view->finalEyeW : 0, view ? view->finalEyeH : 0,
                          view ? view->focusW : 0, view ? view->focusH : 0, view ? "" : " (no focus view this frame)",
                          census.dumpThis ? ", its constant buffers saved to logs\\census" : "");
        }
    }
    if (pass < 2) {  // the tracker's comparison
        tr.actualBegin[pass] = tr.draws;
        tr.actual = (std::max)(tr.actual, pass + 1);
    }
    g.drawFocus = view != nullptr && !g.broken;
    g.passTwinBinds = 0;
    if (pass >= 0 && pass < 2) {  // the focus mask: this pass's scene depth, as bound at its begin
        g.mask.done[pass] = false;
        g.mask.sceneDepth[pass] = nullptr;
        ComPtr<ID3D11DepthStencilView> dsv;
        g.context->OMGetRenderTargets(0, nullptr, &dsv);
        ComPtr<ID3D11Resource> depthTarget;
        if (dsv)
            dsv->GetResource(&depthTarget);
        g.mask.sceneDepth[pass] = depthTarget.Get();
        g.corridor.done[pass] = false;  // the focus corridor: its mask and its copy once each this pass
        g.corridor.copiedThisPass[pass] = false;
        g.corridor.maskedThisPass[pass] = false;
        g.corridor.filledThisPass[pass] = false;
        // DLSS's depth before the transparents: none copied yet; the scene's depth as bound now.
        auto& gl = g.glass;
        gl.copies = 0;
        gl.copied = false;
        gl.opaqueSince = true;
        gl.opaqueAfterCopy = 0;
        gl.sceneBound = gl.mode && depthTarget;
        gl.dsv = gl.sceneBound ? dsv.Get() : nullptr;  // raw: bound now
    }
    g.postWindow = g.focusCopied = g.focusComposited = false;
    g.postTrace.clear();
    g.copyTarget.Reset();
    g.compositeOk = g.focusBlitted = g.overlayTarget = false;
    g.obsCopied = g.obsComposited = g.obsFinal = false;
    std::fill(std::begin(g.obsCopyTargets), std::end(g.obsCopyTargets), nullptr);
    g.obsCopyCount = 0;
    g.observeShape = !g.drawFocus || !g.lastFrameFinals;
    g.stats.shapeSkippedPasses += pass < 2 && !g.observeShape ? 1 : 0;
    g.copyUnknown = g.copyKnownForTests >= 0 ? g.copyKnownForTests == 0 : mvr_probe_yebis_copies() == 0;
    g.blitUnknown = g.blitKnownForTests >= 0 ? g.blitKnownForTests == 0 : mvr_probe_final_blits() == 0;
    if (pass == 0)
        g.finalThisFrame[0] = g.finalThisFrame[1] = false;  // a new frame: its own focus finals
    if (pass == 0 && cb_track()) {  // car (or view) tracking: the matrices drawn from now on are this frame's
        Timed carTimed(g.time.cars);
        ++g.carFrame;
        if (g.carTrack)
            g.cars.frame_begin(g.carFrame);
        if (g.carLightOn)  // night car lighting: this frame's floodlights per car slot
            g.carLights.begin_frame();
        g.carLightsBegun = true;
        g.vbChecked = nullptr;  // a buffer freed since can't pass for one checked before at its address
    }
    if (pass == 1) {  // night car lighting: a frame whose first pass went unseen starts its table here (not on the
                      // frame before's lists and spotlights, the spotlights' buffer rewritten since)
        if (!g.carLightsBegun && g.carLightOn)
            g.carLights.begin_frame();
        g.carLightsBegun = false;
    }
    if (g.carTrack) {  // the pass's draws matched by pass 0's order first (car_motion.h)
        Timed carTimed(g.time.cars);
        g.cars.pass_begin(pass);
    }
    g.afterResolve = false;
    if (pass == 0) {  // this frame's timestamps, in a slot whose last frame has been read
        if (g.gpuSlot >= 0) {  // the frame before stopped short of pass 3: its timing left out
            g.context->End(g.gpu[g.gpuSlot].disjoint.Get());
            g.gpuSlot = -1;
        }
        gpu_collect();
        const int slot = static_cast<int>(g.gpuFrameNo++ % kGpuFrames);
        GpuFrame& f = g.gpu[slot];
        g.gpuSlot = -1;
        if (!f.pending && f.disjoint) {
            for (auto& p : f.stamped)
                for (bool& s : p)
                    s = false;
            for (auto& p : f.corridorStamped)
                for (bool& s : p)
                    s = false;
            f.mode = g.mode;
            g.context->Begin(f.disjoint.Get());
            g.gpuSlot = slot;
        }
    }
    stamp(pass, 0);
    g.replaceResolve = replaceResolve && pass >= 2 && g.keptOk[pass & 1];
    if (pass < 2)
        g.keptOk[pass] = false;
    if (pass == 0) {
        g.sample = sample;
        for (bool& h : g.have)
            h = false;
    }
    if (pass >= 0 && pass < 4)
        g.resolved[pass].Reset();
    if (view) {
        g.view = *view;
        g.viewOf[view->eye & 1] = *view;
        g.context->UpdateSubresource(g.remapCb.Get(), 0, nullptr, view->remap, 0, 0);
        ++g.stats.passesWithFocus;
    }
    // Texture sharpening: this pass's biases (its view's), the twins bound with two views on.
    g.bias = g.biasHooks && g.biasCb && view && (view->mipBias[0] != 0 || view->mipBias[1] != 0);
    g.biasCbBound = false;
    // Car slots: this pass's views not cleared yet, nothing bound (pass end unbound them).
    g.slotsBound = g.slotsCleared = false;
    g.slotViewBound = nullptr;
    g.slotViewsThisPass = 0;
    if (g.blendSwapped)  // a pass that didn't end: the game's blend state back before it is read
        apply_blend();
    g.stats.biasPasses += g.bias ? 1 : 0;
    if (g.biasHooks && g.biasCb) {  // the game's PS b13 as the pass begins (texture sharpening's buffer may go there)
        ID3D11Buffer* b = nullptr;
        g.context->PSGetConstantBuffers(13, 1, &b);
        if (b != g.biasCb.Get())
            hold(g.gamePsCb13, b);
        if (b)
            b->Release();
        g.biasCbUsed = false;
    }
    read_bindings();
    keep_game_vps();
    keep_game_scs();
    const bool cutNow = cut_rects();  // live resolution: the rects the game set before the pass began
    update();
    if (cutNow) {  // (update applies them only when the two views turn on or off)
        apply_viewports();
        if (g.scCount)
            apply_scissors();
    }
}

void mvr_size_cut(bool on, unsigned fullW, unsigned fullH, unsigned eyeW, unsigned eyeH) {
    sizeCut.endedThisPass = false;  // (called before each eye pass begins)
    sizeCut.on = on && fullW && fullH && eyeW && eyeH && (eyeW != fullW || eyeH != fullH);
    sizeCut.fullW = fullW;
    sizeCut.fullH = fullH;
    sizeCut.eyeW = eyeW;
    sizeCut.eyeH = eyeH;
}

MvrSizeCutStats mvr_size_cut_stats() { return {sizeCut.viewports, sizeCut.scissors, sizeCut.left}; }

void mvr_replay_lists_plain(bool on) {
    if (g.listsAsBefore.exchange(!on) != !on)
        simqvfr::logf("lmu mvr: the game's command lists from now on executed %s",
                      on ? "plain (the two views off, their calls passed on: the fix)"
                         : "AS BEFORE THE FIX (the two views as they were, their calls handled as the game's)");
}

void mvr_pass_end(int pass) {
    if (!g.context || g.pass < 0)
        return;
    Timed timed(g.time.hooks);
    slot_unbind();  // car slots: the game's targets and blend state as it bound them
    if (pass >= 0 && pass < 2) {
        if (g.observeShape) {
            g.shapeMissing[pass] = !g.afterResolve ? 1 : !g.obsCopied ? 2 : !g.obsComposited ? 3 : !g.obsFinal ? 4 : 0;
            g.shapeOk[pass] = g.shapeMissing[pass] == 0;
        } else {  // not looked at: its focus final made stands for every step (none: not ready, seen the next frame)
            g.shapeMissing[pass] = 0;
            g.shapeOk[pass] = g.finalThisFrame[pass & 1];
        }
    }
    g.drawFocus = false;
    g.overlayTarget = false;
    update();  // the game's shader, one view, its own viewports
    g.bias = false;
    update_ps();  // the game's pixel shader (update() put it back if two views were on)
    if (g.remapBound) {  // the game's VS b13 in place of our remap
        realVSSetConstantBuffers(g.context, 13, 1, &g.gameVsCb13);
        g.remapBound = false;
    }
    if (g.biasCbUsed) {  // and its PS b13, if texture sharpening's buffer is still there
        ID3D11Buffer* b = nullptr;
        g.context->PSGetConstantBuffers(13, 1, &b);
        if (b == g.biasCb.Get())
            g.context->PSSetConstantBuffers(13, 1, &g.gamePsCb13);
        if (b)
            b->Release();
        g.biasCbUsed = g.biasCbBound = false;
    }
    stamp(pass, 2);
    if (pass == 3 && g.gpuSlot >= 0) {
        g.context->End(g.gpu[g.gpuSlot].disjoint.Get());
        g.gpu[g.gpuSlot].pending = true;
        g.gpuSlot = -1;
    }
    if (g.sample && !g.keepFocus && pass >= 0 && pass < 4) {  // gate C's comparison (gate D's went with the four passes)
        const int eye = pass & 1;
        const MvrPassView& v = g.viewOf[eye];
        if (pass < 2)
            g.have[pass] = copy_region(g.mvrImage[eye], g.resolved[pass].Get(), v.eyeW, 0, v.focusW, v.focusH);
        else
            g.have[pass] = copy_region(g.passImage[eye], g.resolved[pass].Get(), 0, 0, v.focusW, v.focusH);
    }
    if (g.focusCopied && !g.focusComposited) {  // the focus post's copy ran, its composite didn't (no composite seen)
        ++g.stats.focusNoComposite;
        if (g.tracesLogged < 4) {
            ++g.tracesLogged;
            simqvfr::logf("lmu mvr: focus post: pass %d ended without the composite; its draws (DrawIndexed) after the copy, "
                          "pixel shader > target:%s",
                          pass, g.postTrace.empty() ? " none" : g.postTrace.c_str());
        }
    }
    g.focusCopied = false;
    if (pass == 0 && census.active) {
        census.active = false;
        simqvfr::logf("lmu mvr: post census %llu: pass 0 ended, %d draws after its resolve%s", census.taken, census.draws,
                      census.draws > kCensusDraws ? " (the first ones logged)" : "");
    }
    if (pass >= 0 && pass < 2)
        tr.actualEnd[pass] = tr.draws;
    g.pass = -1;
    g.replaceResolve = false;
    g.postWindow = false;
    g.afterResolve = false;
}

void mvr_keep_focus(bool on) { g.keepFocus = on; }

void mvr_ui_mips(bool on) { g.uiMips.on = on; }

void mvr_mip_bias_hooks(bool on) {
    if (!g.context)  // before the install only: its hooks are chosen there
        g.biasHooks = on;
}

bool mvr_mip_bias_ready() { return g.context && g.biasHooks; }

void mvr_car_track(bool on) {
    if (!g.context)  // before the install only: its hooks are chosen there
        g.carTrack = on;
}

void mvr_test_scene_target(ID3D11Resource* scene) {
    tr.scene = scene;
    tr.sceneDesc = {};
    ComPtr<ID3D11Texture2D> t;
    if (scene && SUCCEEDED(scene->QueryInterface(IID_PPV_ARGS(&t))))
        t->GetDesc(&tr.sceneDesc);
}

ID3D11Texture2D* mvr_car_slot_texture() { return g.slotTex.Get(); }

void mvr_car_active(bool on) { g.carActive = on; }
void mvr_eye_off(int eye) { g.eyeOff = eye == 0 || eye == 1 ? eye : -1; }

void mvr_car_slot_images(bool on) { g.slotImages = on; }

void mvr_car_slots(int level) {
    if (!g.context) {  // before the install only (and after mvr_car_track: the slots need the tracking)
        g.slotsLevel = level;
        g.slotsOn = level > 0 && g.carTrack;
    }
}

bool mvr_shape_ready() { return g.shapeReady; }
const char* mvr_shape_missing(int eye) {
    static const char* const kWhat[] = {"every step", "no post-processing begun", "no scene copy", "no composite",
                                        "no final blit", "no pass"};
    return kWhat[std::clamp(g.shapeMissingLast[eye & 1], 0, 5)];
}
void mvr_roles_known_for_tests(int copy, int blit) {
    g.copyKnownForTests = copy;
    g.blitKnownForTests = blit;
    g.copyUnknown = copy == 0;  // (outside a pass too: the scene's target learned by the composite's bindings)
    g.blitUnknown = blit == 0;
}
void mvr_shape_force_bad(bool bad) { g.shapeForceBad = bad; }
void mvr_test_reads_as_game(bool on) { g.readsAsGameForTests = on; }

void mvr_fsr(bool on, ID3D11Resource* eye0, ID3D11Resource* eye1) {
    g.fsr = on;
    g.fsrEye[0] = eye0;
    g.fsrEye[1] = eye1;
}

void mvr_fsr_watch(bool on) { g.fsrWatch = on; }

bool mvr_fsr_written() {
    const bool seen = g.fsrUavSeen;
    g.fsrUavSeen = false;
    return seen;
}

void mvr_viewport_reads(int mode) {
    if (!g.context)  // before the install only: its hooks are chosen there
        g.viewportReads = mode == 0 || mode == 2 ? mode : 1;
}

void mvr_view_track(bool on) {
    if (!g.context)  // before the install only: its hooks are chosen there
        g.viewTrack = on;
}

bool mvr_car_view_pair(int pass, float (&view)[16], float (&viewProj)[16]) {
    if (pass < 0 || pass > 1)
        return false;
    const State::PassPairs& p = g.passPairs[pass];
    if (p.frame != g.carFrame)
        return false;
    int best = -1;
    for (int i = 0; i < p.count; ++i)
        if (p.carDraws[i] && (best < 0 || p.carDraws[i] > p.carDraws[best]))
            best = i;
    if (best < 0)
        return false;
    std::memcpy(view, p.pair[best], sizeof(view));
    std::memcpy(viewProj, p.pair[best] + 16, sizeof(viewProj));
    return true;
}

int mvr_view_pairs(int pass, float (*pairs)[32], unsigned* worldDraws, int max) {
    if (pass < 0 || pass > 1 || !pairs)
        return 0;
    const State::PassPairs& p = g.passPairs[pass];
    if (p.frame != g.carFrame)  // none drawn in this frame's pass yet
        return 0;
    const int n = p.count < max ? p.count : max;
    for (int i = 0; i < n; ++i) {
        std::memcpy(pairs[i], p.pair[i], sizeof(p.pair[i]));
        if (worldDraws)
            worldDraws[i] = p.worldDraws[i];
    }
    return n;
}

bool mvr_scene_size(unsigned& width, unsigned& height) {
    width = tr.sceneSizeW;
    height = tr.sceneSizeH;
    return width && height;
}

void mvr_focus_replay(bool on) {
    g.focusReplay = on;
    if (!on) {  // nothing doubled after the passes any more (R5's fallback switches it off mid-session)
        g.finalThisFrame[0] = g.finalThisFrame[1] = false;
        g.postWindow = false;
    }
}

ID3D11Texture2D* mvr_focus_image(int eye, unsigned long long& composites) {
    composites = g.focusPost.composites(eye);
    return g.focusPost.image(eye);
}

bool mvr_focus_final(int eye, unsigned& x, unsigned& y, unsigned& width, unsigned& height) {
    if (eye < 0 || eye > 1 || !g.finalThisFrame[eye])
        return false;
    const MvrPassView& v = g.viewOf[eye];
    x = final_eye_w(v);  // as focus_blit put it (or FSR's upscale: fsr_final)
    y = 0;
    width = v.finalFocusW ? v.finalFocusW : v.focusW;
    height = v.finalFocusH ? v.finalFocusH : v.focusH;
    return true;
}

bool mvr_focus_ready(int eye) { return g.context && g.keepFocus && g.keptOk[eye & 1]; }

void mvr_mask_stale(const std::atomic<bool>* stale) { g.maskStale = stale; }

void mvr_mask_ab_drop(bool drop) { g.maskAbDrop = drop; }

void mvr_corridor(bool on, bool off, float halfM, float tallM, float startM, float farM, float eyeHeightM, bool show,
                  float roadSlopeDeg, float centreReachM, float featherM) {
    auto& c = g.corridor;
    if (show && !c.show)
        simqvfr::log_line("lmu mvr: corridor show mode: what lies outside the corridor tinted blue in the focus views");
    c.show = show;
    if (!on && c.on)
        simqvfr::log_line("lmu mvr: corridor off: the focus views render all of their box");
    if (on && !c.on)
        simqvfr::logf("lmu mvr: corridor on: the focus views render only what lies within %.1f m either side of the road's "
                      "centre, up to %.1f m above the road beneath it, %.1f m%s ahead (the eye %.2f m above the road); the "
                      "rest of each focus view filled with the eye's view (show mode: tinted instead, nothing skipped)",
                      halfM, tallM, startM, farM > 0 ? (" to " + std::to_string(static_cast<int>(farM)) + " m").c_str()
                                                      : " and further",
                      eyeHeightM);
    c.on = on;
    c.abOff = off;
    c.halfM = halfM;
    c.tallM = tallM;
    c.startM = startM;
    c.farM = farM;
    c.eyeHeightM = eyeHeightM;
    c.roadSlopeDeg = roadSlopeDeg;
    c.centreReachM = centreReachM;
    c.featherM = featherM;
}

void mvr_corridor_band(int pct, int padPct) {
    auto& c = g.corridor;
    if (pct > 0 && c.bandPct <= 0)
        simqvfr::logf("lmu mvr: corridor DLSS band: DLAA on %d%% of each focus view's rows, over the corridor's (%d%% kept "
                      "about them); the other rows the eye's image",
                      pct, padPct);
    if (pct <= 0 && c.bandPct > 0)
        simqvfr::log_line("lmu mvr: corridor DLSS band off: DLAA over the whole focus view");
    c.bandPct = std::clamp(pct, 0, 100);
    c.bandPadPct = std::clamp(padPct, 0, 50);
}

void mvr_corridor_coverage(bool show) {
    auto& c = g.corridor;
    if (show != c.coverage)
        simqvfr::logf("lmu mvr: corridor: its coverage %s", show ? "shown (what it takes from the periphery tinted blue)"
                                                                 : "not shown");
    c.coverage = show;
}

void mvr_corridor_feather_growth(float percent) {
    auto& c = g.corridor;
    const float f = std::clamp(percent, 0.0f, 50.0f) / 100.0f;
    if (f != c.featherGrowth)
        simqvfr::logf("lmu mvr: corridor: its edge's blend grows by %.0f%% of the distance", f * 100);
    c.featherGrowth = f;
}

void mvr_corridor_spread(float degrees) {
    auto& c = g.corridor;
    const float d = std::clamp(degrees, 0.0f, 60.0f);
    if (d != c.spreadDeg)
        simqvfr::logf("lmu mvr: corridor: widening %.0f degrees either side with distance", d);
    c.spreadDeg = d;
}

void mvr_corridor_car(float metres) {
    auto& c = g.corridor;
    const float m = (std::max)(0.0f, metres);
    if (m != c.carM)
        simqvfr::logf("lmu mvr: corridor: the car kept sharp: %s", m > 0 ? ("whatever lies within " +
                      std::to_string(static_cast<int>(m * 100)) + " cm").c_str() : "no, from the eye's image");
    c.carM = m;
}

void mvr_glass_depth(int mode) {
    if (!g.context)  // before the install only: its hooks are chosen there
        g.glass.mode = mode > 0 ? 1 : 0;
}

void mvr_glass_read(bool read) { g.glass.read = read; }

void mvr_focus_mask(bool on, const simqvfr::FocusArea& area, unsigned marginTexels) {
    auto& m = g.mask;
    const bool shape = area.outline() || area.radius > 0 || area.clipped();
    if (m.on != (on && shape) || m.margin != marginTexels) {  // (a new shape logs its mesh as it's made)
        if (on && shape)
            simqvfr::logf("lmu mvr: focus mask on: the focus views' texels %u past the focus shape skip the scene%s",
                          marginTexels, m.failed ? " (its resources failed: stays off)" : "");
        else if (m.on)
            simqvfr::log_line(on ? "lmu mvr: focus mask off: the focus shape is the plain box"
                                 : "lmu mvr: focus mask off ([quad] focus_mask=0)");
    }
    m.on = on && shape;
    m.area = area;
    m.margin = marginTexels;
}

int mvr_mask_dropped() {
    const int n = g.maskDroppedSince;
    g.maskDroppedSince = 0;
    return n;
}

void mvr_panel_draws(PanelFn fn) { g.panelFn = fn; }

ID3D11Resource* mvr_copy_source(ID3D11Resource* dst) {
    if (!dst)
        return nullptr;
    for (unsigned i = 1; i <= State::kCopiesKept; ++i) {
        const State::CopySeen& c = g.copies[(g.copyNext - i) % State::kCopiesKept];
        if (c.dst == dst && c.src && c.frame + 1 >= g.framesSinceInstall)  // this frame's or the one before's
            return c.src;  // held by the ring (render thread: valid until its next copies)
    }
    return nullptr;
}

ID3D11Texture2D* mvr_eye_final(int eye) {
    if (eye < 0 || eye > 1 || !g.eyeFinal[eye])
        return nullptr;
    ComPtr<ID3D11Texture2D> t;
    return SUCCEEDED(g.eyeFinal[eye].As(&t)) ? t.Get() : nullptr;  // raw: eyeFinal holds it
}

void mvr_redraw_panel(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, unsigned width, unsigned height,
                      unsigned vertexCount, unsigned startVertex) {
    if (!context || !rtv || !width || !height)
        return;
    // The game's targets, viewports and scissors kept, put back past the hooks when this returns (a count of 0 set back
    // as 0; the real calls don't look at g.replaying).
    simqvfr::d3d11_state::Spec spec;
    spec.parts = simqvfr::d3d11_state::kOMTargets | simqvfr::d3d11_state::kViewports | simqvfr::d3d11_state::kScissors;
    const simqvfr::d3d11_state::Guard<RealCalls> keep(context, spec);
    const UINT n = keep.viewport_count(), sn = keep.scissor_count();
    const D3D11_VIEWPORT* vps = keep.viewports();
    const D3D11_RECT* scs = keep.scissors();
    // The game's target's size: its viewport and scissors scaled from that to the image's.
    float sx = 1, sy = 1;
    if (ID3D11RenderTargetView* first = keep.rtv(0)) {
        ComPtr<ID3D11Resource> resource;
        first->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC d{};
        if (resource && SUCCEEDED(resource.As(&texture))) {
            texture->GetDesc(&d);
            sx = d.Width ? static_cast<float>(width) / static_cast<float>(d.Width) : 1.0f;
            sy = d.Height ? static_cast<float>(height) / static_cast<float>(d.Height) : 1.0f;
        }
    }
    D3D11_VIEWPORT vp = n ? vps[0] : D3D11_VIEWPORT{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    if (n) {
        vp.TopLeftX *= sx;
        vp.TopLeftY *= sy;
        vp.Width *= sx;
        vp.Height *= sy;
    }
    D3D11_RECT sc = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (sn)
        sc = {static_cast<LONG>(std::lround(scs[0].left * sx)), static_cast<LONG>(std::lround(scs[0].top * sy)),
              static_cast<LONG>(std::lround(scs[0].right * sx)), static_cast<LONG>(std::lround(scs[0].bottom * sy))};
    const bool was = g.replaying;
    g.replaying = true;
    realOMSetRenderTargets(context, 1, &rtv, nullptr);
    realRSSetViewports(context, 1, &vp);
    realRSSetScissorRects(context, 1, &sc);
    call_real<kHookDraw>(context, static_cast<UINT>(vertexCount), static_cast<UINT>(startVertex));
    g.replaying = was;
    ++g.stats.panelReplays;
}
