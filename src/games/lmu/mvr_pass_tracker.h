// Part of mvr_pass.cpp's translation unit (included once, in order): the pass tracker (passes begun and ended by target
// binds, the final blit, the frame end) and the GPU timestamps per pass. Included only by mvr_pass.cpp, inside its
// anonymous namespace (lmu::<anonymous>), after mvr_pass_hook_table.h; never include it anywhere else: the per-draw
// path stays in one translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

void track_begin(const char* what) {
    tr.open = true;
    tr.pass = tr.passes++;
    tr.resolved = tr.blitSeen = tr.postSeen = false;
    tr.finalTarget = tr.sceneTarget = nullptr;
    tr.begin[tr.pass] = tr.draws;
    if (tr.actual == 0 && tr.firstBind.empty())
        tr.firstBind = what;
    if (tr.driveBegin)
        tr.driveBegin(tr.pass);  // R4b: the pass's work (mvr_pass_begin among it) now, before its draws
    if (pt.recording)
        pt_event("P+%d on %s; its focus view %s (eye %ux%u, focus %ux%u)", tr.pass, what,
                 g.drawFocus ? "drawn" : "not drawn", g.view.eyeW, g.view.eyeH, g.view.focusW, g.view.focusH);
}

void track_end(const char* why) {
    if (!tr.open)
        return;
    if (tr.singleSample && !tr.resolved) {  // MSAA off: a pass that never reached YEBIS's copy
        ++tr.stats.passesWithoutPost;
        if (tr.passesWithoutPostLogged < 3) {
            ++tr.passesWithoutPostLogged;
            simqvfr::logf("lmu mvr: pass tracker, MSAA off: frame %llu: pass %d ended (%s) %llu draws in, before its "
                          "post-processing began",
                          static_cast<unsigned long long>(g.framesSinceInstall), tr.pass, why,
                          static_cast<unsigned long long>(tr.draws - tr.begin[tr.pass]));
        }
    }
    if (pt.recording)
        pt_event("P-%d at %s", tr.pass, why);
    tr.open = false;
    tr.end[tr.pass] = tr.draws;
    tr.endWhy[tr.pass] = why;
    if (tr.driveEnd)
        tr.driveEnd(tr.pass);  // R4b: after the pass's draws (mvr_pass_end among it)
}

// A render target bind on the game's immediate context: a pass begins at the first multisample target bound with a
// depth target, or with MSAA off the learned scene target bound with one (after the frame's start or the previous
// pass's end); the open one ends at another target bound after its final blit, or at the next pass's scene (a
// multisample bind after its resolve, other than its own scene's bound again before any YEBIS draw: a stage between
// the resolve and YEBIS does that; with MSAA off only the final blit or the frame end: YEBIS draws into the scene's
// own target then).
void track_bind(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    ComPtr<ID3D11Resource> r0;
    if (n && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && rtvs && rtvs[0])
        rtvs[0]->GetResource(&r0);
    bool msaa = false;
    D3D11_TEXTURE2D_DESC d{};
    if (dsv && r0) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(r0.As(&texture))) {
            texture->GetDesc(&d);
            msaa = d.SampleDesc.Count > 1;
        }
    }
    tr.msaaFrame = tr.msaaFrame || msaa;
    tr.msaaSinceScene = tr.msaaSinceScene || (msaa && tr.scene);
    const bool learned = !msaa && !tr.msaaFrame && !tr.learnedThisFrame && dsv && r0 && tr.scene && r0.Get() == tr.scene &&
                         d.Width == tr.sceneDesc.Width && d.Height == tr.sceneDesc.Height &&
                         d.Format == tr.sceneDesc.Format && d.SampleDesc.Count == tr.sceneDesc.SampleDesc.Count;
    bool multisampleTarget = msaa;  // also without depth (the tracker's own test needs both)
    if (pt.recording && r0 && !dsv) {
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC td{};
        if (SUCCEEDED(r0.As(&texture))) {
            texture->GetDesc(&td);
            multisampleTarget = td.SampleDesc.Count > 1;
        }
    }
    if (pt.recording && (dsv || multisampleTarget)) {
        ComPtr<ID3D11Resource> depthTarget;
        if (dsv)
            dsv->GetResource(&depthTarget);
        const std::string rt = pt_tex(r0.Get()), ds = pt_tex(depthTarget.Get());
        pt_event("B %s depth %s%s", rt.c_str(), ds.c_str(), tr.open && tr.resolved ? " (after a resolve in the pass)" : "");
    }
    if (tr.open) {
        const ID3D11Resource* finalOf = tr.singleSample && tr.finalTarget ? tr.finalTarget : g.eyeFinal[tr.pass].Get();
        if (tr.blitSeen && r0 && r0.Get() != finalOf) {
            track_end("another target after its final blit");
        } else if (msaa && tr.resolved && r0.Get() == tr.sceneTarget && !tr.postSeen) {
            ++tr.stats.sceneRebinds;  // the pass's own scene again, YEBIS not begun: the same pass
            if (!tr.rebindLogged) {
                tr.rebindLogged = true;
                simqvfr::logf("lmu mvr: pass tracker: frame %llu pass %d: its scene target bound again after its resolve, "
                              "before YEBIS (a stage between them): the same pass, not the next one's scene",
                              static_cast<unsigned long long>(g.framesSinceInstall), tr.pass);
            }
        } else if (msaa && tr.resolved) {
            track_end("the next pass's scene");
        }
    }
    if (!tr.open && (msaa || learned) && tr.passes < 2) {
        char what[96];
        std::snprintf(what, sizeof(what), "%ux%u format %d x%u", d.Width, d.Height, static_cast<int>(d.Format),
                      d.SampleDesc.Count);
        tr.sceneSizeW = d.Width;  // the main targets' size, for the pass's work (qvfr_frame.cpp main_size)
        tr.sceneSizeH = d.Height;
        track_begin(what);
        tr.sceneTarget = msaa ? r0.Get() : nullptr;  // raw, compared only
        tr.singleSample = learned;
        tr.stats.singleSamplePasses += learned ? 1 : 0;
    }
    tr.offScene = tr.open && tr.singleSample && !tr.resolved && r0 && !dsv && r0.Get() != tr.scene;
}

// FSR with MSAA off (a render target bind, after fsr_final): there is no final blit (FSR's compute upscale writes the
// eye's image), so the eye's image bound after the pass's post began is its final whether or not our focus step ran
// there (fsr_final marks it only with the focus composite placed): the pass ends at the next other target. Without
// this, a frame with no focus view (recovery's plain frames, one focus failure) left pass 0 open to the frame's end:
// pass 1 taken into it, uncut, and recovery, which retries only on frames with both passes, never tried again
// (review 2026-11-17).
void track_fsr_final(UINT n, ID3D11RenderTargetView* const* rtvs) {
    if (!tr.on || !tr.open || !tr.singleSample || !tr.resolved || tr.blitSeen || !g.fsr || tr.pass < 0 || !n ||
        n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL || !rtvs || !rtvs[0])
        return;
    ComPtr<ID3D11Resource> r0;
    rtvs[0]->GetResource(&r0);
    if (!r0 || r0.Get() != g.fsrEye[tr.pass & 1])
        return;
    tr.blitSeen = true;
    tr.finalTarget = r0.Get();  // raw, compared only
    ++tr.stats.fsrEnds;
    if (!tr.fsrEndLogged) {
        tr.fsrEndLogged = true;
        simqvfr::logf("lmu mvr: pass tracker, MSAA off: frame %llu pass %d: FSR's eye image bound without our focus step: "
                      "the pass's final, it ends at the next target (the first; counted from now on)",
                      static_cast<unsigned long long>(g.framesSinceInstall), tr.pass);
    }
}

// The frame end: the open pass ends; this frame's tracked passes against the game's.
void track_frame_end(unsigned long long frame) {
    track_end("the frame end");
    MvrTrackStats& s = tr.stats;
    ++s.frames;
    if (tr.actual == 0) {
        if (tr.passes) {
            ++s.framesWithoutGame;
            s.passesWithoutGame += static_cast<uint64_t>(tr.passes);
            if (!tr.menuLogged) {
                tr.menuLogged = true;
                simqvfr::logf("lmu mvr: pass tracker: frame %llu has no eye passes of the game's, yet %d found (the first on %s)",
                              frame, tr.passes, tr.firstBind.c_str());
            }
        }
    } else if (tr.passes != tr.actual) {
        ++s.countDiffers;
        if (tr.detailsLogged < 6) {
            ++tr.detailsLogged;
            simqvfr::logf("lmu mvr: pass tracker: frame %llu: %d passes found, the game's %d", frame, tr.passes, tr.actual);
        }
    } else {
        bool ok = true;
        std::string detail;
        for (int q = 0; q < tr.passes; ++q) {
            const long long startGap = static_cast<long long>(tr.begin[q]) - static_cast<long long>(tr.actualBegin[q]);
            const long long endGap = static_cast<long long>(tr.actualEnd[q]) - static_cast<long long>(tr.end[q]);
            if (startGap < 0) {
                ++s.beganBefore;
                ok = false;
            } else {
                s.startGapSum += static_cast<uint64_t>(startGap);
                s.startGapMax = (std::max)(s.startGapMax, static_cast<uint64_t>(startGap));
            }
            if (endGap < 0) {
                ++s.endedAfter;
                ok = false;
            } else {
                s.endGapSum += static_cast<uint64_t>(endGap);
                s.endGapMax = (std::max)(s.endGapMax, static_cast<uint64_t>(endGap));
            }
            ++s.passesCompared;
            char part[200];
            std::snprintf(part, sizeof(part), "%spass %d: began %lld draws into the game's, ended %lld draws before its end (%s)",
                          q ? "; " : "", q, startGap, endGap, tr.endWhy[q] ? tr.endWhy[q] : "?");
            detail += part;
        }
        if (ok)
            ++s.matched;
        if (tr.detailsLogged < 6) {
            ++tr.detailsLogged;
            simqvfr::logf("lmu mvr: pass tracker: frame %llu: %s", frame, detail.c_str());
        }
    }
    tr.passes = tr.actual = 0;
    tr.stats.lastFrameDraws = tr.draws;
    tr.draws = 0;
    tr.firstBind.clear();
    tr.msaaFrame = tr.singleSample = tr.learnedThisFrame = tr.offScene = false;
    tr.stageCandidate = nullptr;
    std::fill(std::begin(tr.drawnWithDepth), std::end(tr.drawnWithDepth), nullptr);
    tr.drawnNext = 0;
}

template <class Shader>
std::string shader_name(Shader* shader);
UINT marked_role(ID3D11DeviceContext* context);
ComPtr<ID3D11Resource> ps_input0(ID3D11DeviceContext* context);
ComPtr<ID3D11Resource> target0(ID3D11DeviceContext* context);
bool scene_sized_copy(ID3D11Resource* target);
bool blit_by_bindings(ID3D11DeviceContext* context);

// MSAA off: after a Draw in a pass begun on the scene's single-sample target, YEBIS's final blit (its pixel shader's
// role, reading the scene's target) marks the pass's end-to-be, whether the focus post ran or not. Only once the
// pass's post-processing has begun (at the copy): the real one comes after it, and a draw like it before the copy
// ended a pass early, splitting an eye's pass in two and leaving the other eye untracked (2026-11-10 19:30, after eye
// tracking went on; logged here to learn what it is).
void note_final_blit(ID3D11DeviceContext* context) {
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    const bool marked = role_of_ps(ps.Get()) == static_cast<UINT>(YebisRole::blit);
    if (!marked && !(tr.resolved && blit_by_bindings(context)))  // (by its bindings only once the post has begun)
        return;
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11Resource> source;
    if (srv)
        srv->GetResource(&source);
    if (!source || source.Get() != tr.scene)
        return;
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    if (!target)
        return;
    if (!tr.resolved) {
        ++tr.stats.blitsBeforePost;
        if (tr.blitsBeforePostLogged < 3) {
            ++tr.blitsBeforePostLogged;
            ComPtr<ID3D11VertexShader> vs;
            context->VSGetShader(&vs, nullptr, nullptr);
            simqvfr::logf("lmu mvr: pass tracker, MSAA off: frame %llu pass %d: a blit (vs %s, ps %s) reads the scene's "
                          "target into %s %llu draws into the pass, before its post-processing began: not its final blit",
                          static_cast<unsigned long long>(g.framesSinceInstall), tr.pass, shader_name(vs.Get()).c_str(),
                          shader_name(ps.Get()).c_str(), describe(target.Get()).c_str(),
                          static_cast<unsigned long long>(tr.draws - tr.begin[tr.pass]));
        }
        return;
    }
    tr.finalTarget = target.Get();  // raw, compared only
    tr.blitSeen = true;
    ++tr.stats.singleSampleBlits;
}

// Outside the tracked passes (the tracker on): render target 0's texture noted when bound with a depth target.
void note_drawn_with_depth(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!dsv || !n || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL || !rtvs || !rtvs[0])
        return;
    ID3D11Resource* r = nullptr;
    rtvs[0]->GetResource(&r);
    if (!r)
        return;
    r->Release();  // raw from here on: compared, never used
    for (ID3D11Resource* seen : tr.drawnWithDepth)
        if (seen == r)
            return;
    tr.drawnWithDepth[tr.drawnNext++ % std::size(tr.drawnWithDepth)] = r;
}

bool drawn_with_depth(ID3D11Resource* r) {
    for (ID3D11Resource* seen : tr.drawnWithDepth)
        if (r && seen == r)
            return true;
    return false;
}

void stamp(int pass, int which) {
    if (g.gpuSlot < 0 || pass < 0 || pass > 3)
        return;
    GpuFrame& f = g.gpu[g.gpuSlot];
    if (f.stamped[pass][which] || !f.stamp[pass][which])
        return;
    g.context->End(f.stamp[pass][which].Get());
    f.stamped[pass][which] = true;
}

// Frames whose timestamps are in: their scene and post times added up.
void gpu_collect() {
    for (GpuFrame& f : g.gpu) {
        if (!f.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (g.context->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        f.pending = false;
        if (dj.Disjoint || !dj.Frequency)
            continue;
        for (int p = 0; p < 4; ++p) {
            UINT64 t[3] = {};
            bool ok = f.stamped[p][0] && f.stamped[p][1] && f.stamped[p][2];
            for (int k = 0; k < 3 && ok; ++k)
                ok = g.context->GetData(f.stamp[p][k].Get(), &t[k], sizeof(t[k]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (!ok || t[1] < t[0] || t[2] < t[1])
                continue;
            g.sceneMs[f.mode][p] += 1000.0 * static_cast<double>(t[1] - t[0]) / static_cast<double>(dj.Frequency);
            g.postMs[f.mode][p] += 1000.0 * static_cast<double>(t[2] - t[1]) / static_cast<double>(dj.Frequency);
            ++g.gpuCounts[f.mode][p];
        }
    }
}
