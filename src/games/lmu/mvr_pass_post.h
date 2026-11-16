// Part of mvr_pass.cpp's translation unit (included once, in order): the focus post's replay at the post-processing
// draws (copy, composite) and the final blit's replay. Included only by mvr_pass.cpp, inside its anonymous namespace
// (lmu::<anonymous>), after mvr_pass_binds.h; never include it anywhere else: the per-draw path stays in one
// translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// After the copy and before a composite is seen: each draw's pixel shader (its checksum, from the probe) and target,
// for the log when the pass ends without one ("r" marks the resolve's target); a draw into the resolve's target with
// a shader that isn't the composite we know logged with its inputs, once per shader.
void trace_post_draw(ID3D11DeviceContext* context, ID3D11PixelShader* ps) {
    uint32_t c[4] = {};
    UINT size = sizeof(c);
    const bool named = ps && SUCCEEDED(ps->GetPrivateData(kShaderChecksumGuid, &size, c)) && size == sizeof(c);
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    const bool intoResolved = target && target.Get() == g.resolved[g.pass].Get();
    if (g.postTrace.size() < 3000) {
        char s[96];
        std::snprintf(s, sizeof(s), " %08x%s>%s%s", named ? c[0] : 0, named ? "" : "?", describe(target.Get()).c_str(),
                      intoResolved ? " r" : "");
        g.postTrace += s;
    }
    if (!intoResolved)
        return;
    ++g.stats.focusUnknownComposites;
    for (int i = 0; i < g.unknownCount; ++i)
        if (std::memcmp(g.unknown[i], c, sizeof(c)) == 0)
            return;
    if (g.unknownCount == static_cast<int>(std::size(g.unknown)))
        return;
    std::memcpy(g.unknown[g.unknownCount++], c, sizeof(c));
    std::string inputs;
    ID3D11ShaderResourceView* srvs[8] = {};
    context->PSGetShaderResources(0, 8, srvs);
    for (int i = 0; i < 8; ++i) {
        ComPtr<ID3D11Resource> r;
        if (srvs[i]) {
            srvs[i]->GetResource(&r);
            srvs[i]->Release();
        }
        if (r)
            inputs += " t" + std::to_string(i) + " " + describe(r.Get()) + (r.Get() == g.resolved[g.pass].Get() ? " r" : "");
    }
    ID3D11SamplerState* samplers[8] = {};
    context->PSGetSamplers(0, 8, samplers);
    int samplerCount = 0;
    for (ID3D11SamplerState* s : samplers)
        if (s) {
            ++samplerCount;
            s->Release();
        }
    simqvfr::logf("lmu mvr: focus post: pass %d: after the copy, pixel shader %08x-%08x-%08x-%08x%s draws into the "
                  "resolve's target (%s), not the composite we know; inputs:%s; %d samplers%s",
                  g.pass, c[0], c[1], c[2], c[3], named ? "" : " (not named: created before the probe?)",
                  describe(target.Get()).c_str(), inputs.c_str(), samplerCount,
                  g.unknownCount == static_cast<int>(std::size(g.unknown)) ? "; no more of these logged" : "");
}

// After the copy: YEBIS's composite by what it does, whatever its tone curve (LMU switches between variants, e.g. in the
// pits and on track: 60678228, b7eb8877, 2026-11-09): a draw into the resolve's target reading the copy's output (t0),
// a glare (t1) and the 1x1 exposure (t2).
bool composite_by_bindings(ID3D11DeviceContext* context) {
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    if (!target || target.Get() != g.resolved[g.pass].Get() || !g.copyTarget)
        return false;
    ID3D11ShaderResourceView* srvs[3] = {};
    context->PSGetShaderResources(0, 3, srvs);
    ComPtr<ID3D11Resource> r[3];
    for (int i = 0; i < 3; ++i)
        if (srvs[i]) {
            srvs[i]->GetResource(&r[i]);
            srvs[i]->Release();
        }
    ComPtr<ID3D11Texture2D> exposure;
    D3D11_TEXTURE2D_DESC ed{};
    if (!r[0] || r[0].Get() != g.copyTarget.Get() || !r[1] || !r[2] || FAILED(r[2].As(&exposure)))
        return false;
    exposure->GetDesc(&ed);
    return ed.Width == 1 && ed.Height == 1;
}

void focus_post_draw(ID3D11DeviceContext* context) {
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    UINT role = role_of_ps(ps.Get());
    if (!role && !g.focusCopied && g.copyUnknown) {
        // No copy shader known by its checksum (a game update compiled it anew): the copy by what it binds. A draw
        // reading the resolve's target into a texture of its size, over the eye only (over the focus view too it is
        // a stage's image: note_post_stage has it), or a draw reading that stage's image.
        const ComPtr<ID3D11Resource> source = ps_input0(context);
        const bool fromResolve = source && source.Get() == g.resolved[g.pass].Get() &&
                                 scene_sized_copy(target0(context).Get()) && !g.stageCovers;
        const bool fromStage = source && g.stageImage && source.Get() == g.stageImage && g.stageCovers &&
                               target0(context).Get() != g.resolved[g.pass].Get();
        if (fromResolve || fromStage) {
            role = static_cast<UINT>(YebisRole::copy);
            ++g.stats.focusCopiesByBindings;
            if (!g.byBindingsLogged) {
                g.byBindingsLogged = true;
                simqvfr::logf("lmu mvr: focus post: pass %d: no YEBIS copy shader known by its checksum: the copy by its "
                              "bindings (a draw reading %s)",
                              g.pass, fromResolve ? "the resolve's target into a texture of its size"
                                                  : "a stage's image of the scene");
            }
        }
    }
    if (g.focusCopied && !g.focusComposited && role != static_cast<UINT>(YebisRole::composite)) {
        if (composite_by_bindings(context)) {
            role = static_cast<UINT>(YebisRole::composite);
            ++g.stats.focusCompositesByBindings;
            uint32_t c[4] = {};
            UINT cs = sizeof(c);
            if (ps && SUCCEEDED(ps->GetPrivateData(kShaderChecksumGuid, &cs, c)) && cs == sizeof(c)) {
                bool seen = false;
                for (int i = 0; i < g.bindingCount && !seen; ++i)
                    seen = std::memcmp(g.byBindings[i], c, sizeof(c)) == 0;
                if (!seen && g.bindingCount < static_cast<int>(std::size(g.byBindings))) {
                    std::memcpy(g.byBindings[g.bindingCount++], c, sizeof(c));
                    simqvfr::logf("lmu mvr: focus post: pass %d: YEBIS's composite by its bindings (the copy's output, a "
                                  "glare, the 1x1 exposure into the resolve's target): pixel shader "
                                  "%08x-%08x-%08x-%08x",
                                  g.pass, c[0], c[1], c[2], c[3]);
                }
            }
        } else {
            trace_post_draw(context, ps.Get());
        }
    }
    if (!role)
        return;
    const int eye = g.pass & 1;
    if (role == static_cast<UINT>(YebisRole::copy) && !g.focusCopied) {
        ComPtr<ID3D11ShaderResourceView> srv;
        context->PSGetShaderResources(0, 1, &srv);
        ComPtr<ID3D11Resource> resource;
        if (srv)
            srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture, resolvedTexture;
        if (!resource || FAILED(resource.As(&texture)) || !g.resolved[g.pass] ||
            FAILED(g.resolved[g.pass].As(&resolvedTexture)))
            return;
        D3D11_TEXTURE2D_DESC d, rd;
        texture->GetDesc(&d);
        resolvedTexture->GetDesc(&rd);
        const bool fromResolve = resource.Get() == g.resolved[g.pass].Get();
        // Not the resolve: YEBIS's copy starting from an image of the resolve's size, made by a stage between the
        // resolve and YEBIS (the composite then reads that image itself, with no copy of the resolve before it). Its
        // downsamples read smaller textures.
        if (!fromResolve && (d.SampleDesc.Count != 1 || d.Width != rd.Width || d.Height != rd.Height))
            return;
        const FocusPostRegion region{g.view.eyeW, 0, g.view.focusW, g.view.focusH};
        if (fromResolve) {
            ComPtr<ID3D11RenderTargetView> copyRtv;
            context->OMGetRenderTargets(1, &copyRtv, nullptr);
            g.copyTarget.Reset();
            if (copyRtv)
                copyRtv->GetResource(&g.copyTarget);  // the composite reads it (composite_by_bindings)
            g.replaying = true;
            g.focusCopied = g.focusPost.after_copy(context, eye, srv.Get(), d.Width, d.Height, region);
            g.replaying = false;
        } else {
            // The focus view's scene: that image's part where the stage's draw covered the focus view, else the
            // resolve's (the stage left out of the focus view, its picture still right). Copied as it is: this copy is
            // a downsample's, its constants not the scene's.
            g.copyTarget = resource;  // what the composite reads (composite_by_bindings)
            const bool fromStage = resource.Get() == g.stageImage && g.stageCovers;
            ComPtr<ID3D11ShaderResourceView> source = srv;
            if (!fromStage) {
                ComPtr<ID3D11Device> device;
                context->GetDevice(&device);
                source.Reset();
                if (device)
                    device->CreateShaderResourceView(g.resolved[g.pass].Get(), nullptr, &source);
            }
            g.replaying = true;
            g.focusCopied = source && g.focusPost.scene_from(context, eye, source.Get(), d.Width, d.Height, region);
            g.replaying = false;
            ++(fromStage ? g.stats.focusStageCopies : g.stats.focusStageResolves);
            bool& logged = fromStage ? g.stageImageLogged : g.stageResolveLogged;
            if (!logged) {
                logged = true;
                char maker[96] = "its draw not seen";
                if (resource.Get() == g.stageImage)
                    std::snprintf(maker, sizeof(maker), "pixel shader %08x-%08x-%08x-%08x, its viewport %s the focus view",
                                  g.stagePs[0], g.stagePs[1], g.stagePs[2], g.stagePs[3],
                                  g.stageCovers ? "over" : "not over");
                simqvfr::logf("lmu mvr: focus post: pass %d: YEBIS's copy starts from %s, not the resolve: a stage "
                              "between them made it (%s); the focus view's scene from %s%s",
                              g.pass, describe(resource.Get()).c_str(), maker, fromStage ? "that image" : "the resolve",
                              g.focusCopied ? "" : " (failed)");
            }
        }
        if (g.focusCopied) {
            ++g.stats.focusCopies;
        } else {
            ++g.stats.focusFailures;
            g.postWindow = false;
        }
    } else if (role == static_cast<UINT>(YebisRole::composite) && g.focusCopied && !g.focusComposited) {
        g.focusComposited = true;
        g.postWindow = false;  // nothing more this pass
        ComPtr<ID3D11ShaderResourceView> glare;
        context->PSGetShaderResources(1, 1, &glare);
        ComPtr<ID3D11Resource> resource;
        if (glare)
            glare->GetResource(&resource);
        ComPtr<ID3D11Texture2D> glareTexture, scene;
        D3D11_TEXTURE2D_DESC gd{}, sd{};
        bool ok = resource && SUCCEEDED(resource.As(&glareTexture)) && g.resolved[g.pass] &&
                  SUCCEEDED(g.resolved[g.pass].As(&scene));
        if (ok) {
            glareTexture->GetDesc(&gd);
            scene->GetDesc(&sd);
            // The eye's glare is the eye's share of the glare texture (YEBIS builds it at the scene's UV: glare_math.h).
            const simqvfr::ViewRegion region = yebis_pass_region(
                {0, 0, static_cast<int>(gd.Width), static_cast<int>(gd.Height)},
                static_cast<double>(g.view.eyeW) / sd.Width, static_cast<double>(g.view.eyeH) / sd.Height);
            g.replaying = true;
            ok = g.focusPost.after_composite(context, eye, region, glare_mapping(g.view.box));
            g.replaying = false;
        }
        g.compositeOk = ok;
        if (ok && g.fsr && !fsr_place(eye))
            g.compositeOk = false;  // not beside the eye: no focus final from FSR's image (fsr_final)
        ++(ok ? g.stats.focusComposites : g.stats.focusFailures);
    }
}

// R3b: right after the game's final blit in a pass that made its focus composite (its pixel shader known by checksum,
// reading the resolve's target into the eye's final): the same quad once more over the focus view's region beside the
// eye (FocusPost::blit_final), with the game's vertex colour. From then on the pass's draws into the eye's final are
// overlays, doubled by MVR (update(), overlay_target).
void focus_blit(ID3D11DeviceContext* context, UINT count, UINT start) {
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    const bool marked = role_of_ps(ps.Get()) == static_cast<UINT>(YebisRole::blit);
    if (!marked && !blit_by_bindings(context))
        return;
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11Resource> source;
    if (srv)
        srv->GetResource(&source);
    if (!source || source.Get() != g.resolved[g.pass].Get())
        return;  // the blit of something else (the desktop mirror's is outside the passes)
    g.stats.focusBlitsByBindings += marked ? 0 : 1;
    g.focusBlitted = true;  // tried once a pass
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    ComPtr<ID3D11Texture2D> texture;
    if (rtv)
        rtv->GetResource(&target);
    D3D11_TEXTURE2D_DESC d{};
    if (target && SUCCEEDED(target.As(&texture)))
        texture->GetDesc(&d);
    const MvrPassView& v = g.view;
    const int eye = g.pass & 1;  // as the focus post's (after_copy, after_composite)
    // The eye's corner of its final: its rendered corner, stretched when the game's render scale isn't 1 (the blit maps
    // the whole main target over the whole final). The focus final beside it at the focus image's own size.
    const unsigned ew = final_eye_w(v), eh = final_eye_h(v);
    if (!texture || ew + v.focusW > d.Width || v.focusH > d.Height) {
        ++g.stats.focusFailures;
        if (!g.blitRefusalLogged) {
            g.blitRefusalLogged = true;
            simqvfr::logf("lmu mvr: focus post: pass %d: the eye's final (%ux%u) has no room for the focus view %ux%u beside "
                          "the eye's %u: no focus final there",
                          g.pass, d.Width, d.Height, v.focusW, v.focusH, ew);
        }
        return;
    }
    g.replaying = true;
    const bool ok = g.focusPost.blit_final(context, eye, count, start, {0, 0, ew, eh}, {ew, 0, v.focusW, v.focusH});
    g.replaying = false;
    if (!ok) {
        ++g.stats.focusFailures;
        return;
    }
    ++g.stats.focusBlits;
    if (tr.open)
        tr.blitSeen = true;
    g.eyeFinal[eye] = target;
    g.finalW[eye] = d.Width;
    g.finalH[eye] = d.Height;
    g.finalThisFrame[eye] = true;
    g.overlayTarget = true;  // the eye's final is bound
    update();
}
