// Part of mvr_pass.cpp's translation unit (included once, in order): the frame's end (mvr_frame_end) and the remaining
// public API. Included only by mvr_pass.cpp, inside namespace lmu, after its anonymous namespace closes, after
// mvr_pass_api.h; never include it anywhere else: the per-draw path stays in one translation unit (no whole-program
// optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// Night car lighting's costs, for the overlay (any thread): GPU and CPU ms per frame, NaN before the first average.
std::atomic<float> carLightGpu{std::numeric_limits<float>::quiet_NaN()}, carLightCpu{std::numeric_limits<float>::quiet_NaN()};

// Night car lighting's frame end: the collection's time-stamp ticks in ms (the counter's rate taken from QPC frame to
// frame), the pass's GPU timings read back, its averages for the overlay, a log line every 900 frames.
void car_light_frame_end() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const uint64_t tsc = __rdtsc();
    if (g.tscMark && now.QuadPart > g.qpcMark.QuadPart && tsc > g.tscMark) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        const double ms = 1000.0 * static_cast<double>(now.QuadPart - g.qpcMark.QuadPart) / static_cast<double>(f.QuadPart);
        if (ms > 1) {
            const double rate = static_cast<double>(tsc - g.tscMark) / ms;
            g.tscPerMs = g.tscPerMs > 0 ? g.tscPerMs * 0.95 + rate * 0.05 : rate;
        }
    }
    g.tscMark = tsc;
    g.qpcMark = now;
    if (g.tscPerMs > 0)
        g.carLightPass.add_collect_ms(static_cast<double>(g.carLightTicks) / g.tscPerMs);
    g.carLightTicks = 0;
    const unsigned long long before = g.carLightPass.windows();
    g.carLightPass.frame_end(g.context);
    const unsigned long long windows = g.carLightPass.windows();
    if (windows == before)
        return;
    carLightGpu.store(static_cast<float>(g.carLightPass.gpu_ms()), std::memory_order_relaxed);
    carLightCpu.store(static_cast<float>(g.carLightPass.cpu_ms()), std::memory_order_relaxed);
    if (windows % 10 == 1) {  // the first average, then every 900 frames (debug logs: why the runs went as they did)
        const CarLightPass::Reasons r = g.carLightPass.take_reasons();
        simqvfr::logd("lmu car light: runs: %llu relit, %llu night vision's colour alone (no car part %llu, no camera "
                      "%llu, no depth view %llu, depth and slots' samples differ %llu, camera singular %llu); the last "
                      "upload: %u slots drawn, %u with floodlights, %u paint, %u the game's normal, %u spotlights",
                      r.relit, r.alone, r.noCar, r.noCamera, r.noDepth, r.samples, r.singular, r.slots, r.lit, r.paint,
                      r.normal, r.spotted);
        unsigned long long outOfReach = 0, kept = 0;
        g.carLights.take_spot_counts(outOfReach, kept);
        const auto& sw = g.spotWhy;
        simqvfr::logd("lmu car light: headlights: %llu spotlight-reading part draws (buffer not ours %llu, count elsewhere "
                      "%llu, buffer not written %llu, per-draw bytes not read %llu), %llu read with %llu spotlights listed; "
                      "%llu kept, %llu out of reach",
                      sw.seen, sw.notBound, sw.otherLayout, sw.notValid, sw.notRead, sw.given, sw.listed, kept, outOfReach);
        g.spotWhy = {};
    }
    if (windows % 10 == 1)  // the first average, then every 900 frames
        simqvfr::logf("lmu car light: %s; %.3f ms GPU and %.3f ms CPU per frame (the last 90 frames); %llu passes relit, "
                      "%zu floodlights this frame, %llu car part draws given lights, %llu without (no light buffer read "
                      "yet or no camera)%s",
                      g.carLightOn ? simqvfr::car_lighting_log(g.carLightSettings).c_str() : "paused",
                      g.carLightPass.gpu_ms(), g.carLightPass.cpu_ms(), static_cast<unsigned long long>(g.carLightRuns),
                      g.carLights.lights().size(), static_cast<unsigned long long>(g.carLightRecords),
                      static_cast<unsigned long long>(g.carLightMisses), g.carLightPass.failed() ? "; THE PASS FAILED" : "");
}

void mvr_frame_end(unsigned long long frame) {
    if (!g.context)
        return;
    if (census.level >= 4) {  // level 4: from this frame's end to the next pass 0's end, every 900 frames, 4 in all
        if (census.pre) {  // no scene this frame (menus): not counted, tried again at the next frame end
            census.pre = false;
            --census.dumps;
            census.next = 0;
            simqvfr::logd("lmu mvr: pass census %llu: a frame ended without pass 0 (%d draws)", census.taken, census.draws);
        }
        // (only after a frame that rendered the scene: the next one likely does too; never in menus)
        if (census.dumps < kCensusDumps && g.framesSinceInstall >= census.next && tr.passes >= 2) {
            census.pre = true;
            census.active = false;
            census.draws = 0;
            ++census.taken;
            ++census.dumps;
            censusSaved.clear();
            census.next = g.framesSinceInstall + kCensusEvery;
            simqvfr::logf("lmu mvr: pass census %llu: frame %llu ended: every draw from here to the end of the next pass 0 "
                          "follows",
                          census.taken, frame);
        }
    }
    if (g.gpuSlot >= 0) {  // a frame of fewer than four passes (two, with the focus finals): its timing complete now
        g.context->End(g.gpu[g.gpuSlot].disjoint.Get());
        g.gpu[g.gpuSlot].pending = true;
        g.gpuSlot = -1;
    }
    if (tr.on) {
        const int passes = tr.passes;
        track_frame_end(frame);  // a pass the tracker ends here reads its focus final (mvr_focus_final) first
        pt_frame_end(frame, passes);
    } else {  // the counts start afresh each frame either way
        tr.passes = tr.actual = 0;
        tr.draws = 0;
    }
    g.lastFrameFinals = g.finalThisFrame[0] && g.finalThisFrame[1];
    g.finalThisFrame[0] = g.finalThisFrame[1] = false;  // the next frame's are its own (a menu frame has none)
    g.shapeReady = g.shapeOk[0] && g.shapeOk[1] && !g.shapeForceBad;
    if (g.copyKnownForTests < 0)
        g.copyUnknown = mvr_probe_yebis_copies() == 0;
    g.stats.shapeFrames += g.shapeOk[0] && g.shapeOk[1] ? 1 : 0;
    for (int e = 0; e < 2; ++e) {
        g.shapeMissingLast[e] = g.shapeOk[e] ? 0 : g.shapeMissing[e] ? g.shapeMissing[e] : 5;
        g.shapeMissing[e] = 0;
    }
    g.shapeOk[0] = g.shapeOk[1] = false;
    for (State::SceneBind& b : g.sceneBinds)  // DLAA's: nothing of the game's held between frames
        b = State::SceneBind{};
    for (State::CopySeen& c : g.copies)  // the copies mvr_copy_source no longer gives: their sources given back
        if (c.src && c.frame + 1 < g.framesSinceInstall) {
            hold(c.src, static_cast<ID3D11Resource*>(nullptr));
            c.dst = nullptr;
        }
    g.sceneRtvChecked = nullptr;  // is_scene_rtv's cache: a view's address may be another's after targets are remade
    g.blendLook = {};             // likewise the blend twin's and the depth state's
    g.depthLook = {};
    // mvr_time_hooks: this frame was timed (counted for the per-frame figures); the next one is if it is 1 in 16.
    g.timedFrames += timeHooks ? 1 : 0;
    timeHooks = timeHooksOn && (frame + 1) % kTimedEvery == 0;
    // The overlay trace (debug logs only): the frames 600, 2400 and 4200 frame ends after the install, each from the
    // frame end before.
    ++g.framesSinceInstall;
    if (g.tracing) {
        g.tracing = false;
        simqvfr::logf("lmu mvr trace: frame %llu traced (from the frame end before): %d draws logged, %d not (over 800); "
                      "the layer's own draws on the game's context (composition, overlay) may be among the outside ones",
                      g.gpuFrameNo, g.traceDraws, g.traceDropped);
    }
    constexpr unsigned long long kTraceAt[] = {600, 2400, 4200};
    if (g.focusReplay && simqvfr::log_debug() && g.framesTraced < static_cast<int>(std::size(kTraceAt)) &&
        g.framesSinceInstall + 1 == kTraceAt[g.framesTraced]) {
        g.tracing = true;
        ++g.framesTraced;
        g.traceDraws = g.traceDropped = 0;
    }
    if (g.sample) {
        g.sample = false;
        if (!g.keepFocus)
            compare_sample(frame);
    }
    if (g.carLight)
        car_light_frame_end();
    if (frame >= g.lastStatsFrame + 900) {
        const double frames = static_cast<double>(frame - g.lastStatsFrame);
        g.lastStatsFrame = frame;
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        const unsigned long long tsc = __rdtsc();
        const double ms = 1000.0 * static_cast<double>(now.QuadPart - g.qpcAtLog) / static_cast<double>(f.QuadPart);
        const double timed = static_cast<double>(g.timedFrames);
        if (timeHooksOn && ms > 0 && tsc > g.tscAtLog && frames > 0 && timed > 0) {
            const double perMs = static_cast<double>(tsc - g.tscAtLog) / ms;
            const auto per = [&](uint64_t ticks) { return static_cast<double>(ticks) / perMs / timed; };
            simqvfr::logf("lmu mvr: this code's CPU in the context hooks %.3f ms per frame, the calls passed on included "
                          "(twin lookups %.3f: %llu, %llu not cached; stereo switches %.3f); %.0f hooked calls per frame; "
                          "%.0f of %.0f frames timed (1 in %llu)",
                          per(g.time.hooks), per(g.time.lookups),
                          static_cast<unsigned long long>(g.stats.lookups - g.loggedLookups),
                          static_cast<unsigned long long>(g.stats.lookupMisses - g.loggedMisses), per(g.time.switches),
                          static_cast<double>(g.time.calls) / frames, timed, frames, kTimedEvery);  // (counted every frame)
            simqvfr::logf("lmu mvr: the pass tracker's and the frame shape's looks at the draws %.3f ms per frame (not in "
                          "the hooks' figure above; the draws passed on not included); the post-processing's start (DLSS, "
                          "the motions' upload, the focus post's window) %.3f, taken out of both figures; pixel shader "
                          "roles: %llu looked up, %llu not cached, %llu by the game's bind (no runtime call); %llu eye "
                          "passes' shape not looked at (focus views drawn after a frame with both)",
                          per(g.time.tracker), per(g.time.post),
                          static_cast<unsigned long long>(g.stats.roleLookups - g.loggedRoleLookups),
                          static_cast<unsigned long long>(g.stats.roleLookupMisses - g.loggedRoleMisses),
                          static_cast<unsigned long long>(g.stats.rolesByBind - g.loggedRolesByBind),
                          static_cast<unsigned long long>(g.stats.shapeSkippedPasses - g.loggedShapeSkipped));
            if (g.carTrack)
                simqvfr::logf("lmu mvr: car tracking's own CPU %.3f ms per frame (its matrix reads %.3f, its draws %.3f, "
                              "the frame's start %.3f; 1 in 16 of its draws and of its matrix reads timed, each less the "
                              "timing's own %llu ticks; not the calls passed on, nor the detours' own cost per call); %.0f "
                              "calls of its hooks per frame (Map, Unmap, VSSetConstantBuffers, the input assembler's "
                              "buffers)",
                              per(g.time.cars + g.time.carReads + g.time.carDraws), per(g.time.carReads),
                              per(g.time.carDraws), per(g.time.cars), static_cast<unsigned long long>(timedEmptyTicks),
                              static_cast<double>(g.time.carCalls) / timed);
            if (g.carTrack)
                simqvfr::logf("lmu mvr: car tracking's hooks' calls in all, the calls passed on included, %.3f ms per "
                              "frame (1 in 16 timed): the most their detours themselves can cost",
                              per(g.time.carHooks));
            if (g.slotsOn)
                simqvfr::logf("lmu mvr: car slots' CPU per frame, the calls they make included: slot views %.3f ms (1 in "
                              "16 timed), the target's binds and the blend states' twins %.3f, the motions computed and "
                              "uploaded %.3f",
                              per(g.time.slotViews), per(g.time.slotState), per(g.time.motion));
        }
        g.time = HookTime{};
        g.timedFrames = 0;
        g.loggedRolesByBind = g.stats.rolesByBind;
        g.loggedLookups = g.stats.lookups;
        g.loggedMisses = g.stats.lookupMisses;
        g.loggedRoleLookups = g.stats.roleLookups;
        g.loggedRoleMisses = g.stats.roleLookupMisses;
        g.loggedShapeSkipped = g.stats.shapeSkippedPasses;
        g.qpcAtLog = now.QuadPart;
        g.tscAtLog = tsc;
        const MvrPassStats& s = g.stats;
        simqvfr::logf("lmu mvr: %llu passes drew their focus view; shader binds %llu twin / %llu plain, %llu stereo "
                      "switches (%llu refused), viewports %llu and scissors %llu doubled; focus regions kept %llu, "
                      "resolves replaced %llu; twin lookups %llu, %llu not cached",
                      static_cast<unsigned long long>(s.passesWithFocus), static_cast<unsigned long long>(s.twinBinds),
                      static_cast<unsigned long long>(s.plainBinds), static_cast<unsigned long long>(s.toggles),
                      static_cast<unsigned long long>(s.stereoErrors), static_cast<unsigned long long>(s.viewportRewrites),
                      static_cast<unsigned long long>(s.scissorRewrites), static_cast<unsigned long long>(s.keptRegions),
                      static_cast<unsigned long long>(s.replacedResolves), static_cast<unsigned long long>(s.lookups),
                      static_cast<unsigned long long>(s.lookupMisses));
        if (g.biasHooks)
            simqvfr::logf("lmu mvr: texture sharpening: %llu passes with a mip bias; pixel shader twin binds %llu, binds of "
                          "shaders without one %llu; twin lookups %llu, %llu not cached; its buffer found unbound %llu "
                          "times (rebound); focus rectangle %.0f,%.0f-%.0f,%.0f, bias focus %.2f, eye %.2f now",
                          static_cast<unsigned long long>(s.biasPasses), static_cast<unsigned long long>(s.psTwinBinds),
                          static_cast<unsigned long long>(s.psNoTwin), static_cast<unsigned long long>(s.psLookups),
                          static_cast<unsigned long long>(s.psLookupMisses), static_cast<unsigned long long>(s.biasCbLost),
                          g.biasConstants[0], g.biasConstants[1], g.biasConstants[2], g.biasConstants[3],
                          g.biasConstants[4], g.biasConstants[5]);
        if (g.carTrack) {  // since the line before, per frame; the keys held now
            const CarMotionStats c = g.cars.stats();
            const CarMotionStats& b = g.carLogged;
            const double n = c.frames > b.frames ? static_cast<double>(c.frames - b.frames) : 1.0;
            const auto pf = [&](uint64_t now, uint64_t before) { return static_cast<double>(now - before) / n; };
            simqvfr::logf("lmu mvr: car tracking: %llu frames; per frame %.1f draws with a world matrix (%.1f the identity, "
                          "%.1f of objects: %.1f matched to the frame before, %.1f first seen or after a gap, %.2f without "
                          "a free slot); %llu keys held, %.1f freed per frame; per frame the per-draw buffer learned %.2f "
                          "times, matrices read %.1f (%.1f through our copy), draws without one %.2f, at another offset %.2f, from a dynamic vertex "
                          "buffer %.1f (keyed without their base vertex); vertex buffers' usage asked %.1f; part draws "
                          "matched by pass 0's order %.1f%% (looked up %.1f out of order, %.1f without an order)",
                          static_cast<unsigned long long>(c.frames - b.frames), pf(c.draws, b.draws),
                          pf(c.identityDraws, b.identityDraws), pf(c.partDraws, b.partDraws), pf(c.matched, b.matched),
                          pf(c.newKeys, b.newKeys), pf(c.full, b.full), static_cast<unsigned long long>(c.live),
                          pf(c.evicted, b.evicted), pf(s.carBufferLearned, g.carStatsLogged.carBufferLearned),
                          pf(s.carMatrixReads, g.carStatsLogged.carMatrixReads),
                          pf(s.carShadowWrites, g.carStatsLogged.carShadowWrites),
                          pf(s.carNoMatrix, g.carStatsLogged.carNoMatrix),
                          pf(s.carOtherOffset, g.carStatsLogged.carOtherOffset),
                          pf(s.carDynamicVb, g.carStatsLogged.carDynamicVb),
                          pf(s.carVbAsked, g.carStatsLogged.carVbAsked),
                          c.partDraws > b.partDraws ? 100.0 * static_cast<double>(c.inOrder - b.inOrder) /
                                                          static_cast<double>(c.partDraws - b.partDraws)
                                                    : 0.0,
                          pf(c.outOfOrder, b.outOfOrder), pf(c.noOrder, b.noOrder));
            g.carLogged = c;
            g.carStatsLogged = s;
        }
        if (g.slotsOn)
            simqvfr::logf("lmu mvr: car slots: %llu passes with the slot target, %llu binds of it, %llu slot views bound, "
                          "%llu slot twin binds, %llu binds of shaders without one while it was bound; %llu blend twins; "
                          "%llu motion uploads (%llu part motions); %llu gate images",
                          static_cast<unsigned long long>(s.slotPasses), static_cast<unsigned long long>(s.slotBinds),
                          static_cast<unsigned long long>(s.slotViews), static_cast<unsigned long long>(s.slotTwinBinds),
                          static_cast<unsigned long long>(s.slotNoTwin), static_cast<unsigned long long>(s.blendTwins),
                          static_cast<unsigned long long>(s.motionUploads), static_cast<unsigned long long>(s.motionSlots),
                          static_cast<unsigned long long>(s.slotImages));
        if (g.glass.mode) {
            const double passes = s.glassPasses ? static_cast<double>(s.glassPasses) : 1.0;
            simqvfr::logf("lmu mvr: depth before the transparents: %llu eye passes with a copy (%.2f copies each, the "
                          "first after %.0f of the pass's %.0f draws on average); %.1f blended draws writing the scene's "
                          "depth per pass, %.2f unblended ones after its last copy (their depth not in DLSS's); %llu "
                          "copies refused, %llu past %d a pass",
                          static_cast<unsigned long long>(s.glassPasses), static_cast<double>(s.glassCopies) / passes,
                          static_cast<double>(s.glassFirstAt) / passes, static_cast<double>(s.glassPassDraws) / passes,
                          static_cast<double>(s.glassBlendedWriters) / passes,
                          static_cast<double>(s.glassOpaqueAfter) / passes, static_cast<unsigned long long>(s.glassRefused),
                          static_cast<unsigned long long>(s.glassCapped), kGlassCopies);
        }
        if (tr.on) {
            const MvrTrackStats& ts = tr.stats;
            const uint64_t compared = ts.passesCompared ? ts.passesCompared : 1;
            simqvfr::logf("lmu mvr: pass tracker: %llu frames: the game's two passes found as its own in %llu (began %llu "
                          "times before the game's, ended %llu after; draws of the game's pass left out at the start %.1f "
                          "on average, %llu at most, at the end %.1f, %llu at most); %llu frames with another count; frames "
                          "without the game's passes: %llu with %llu found",
                          static_cast<unsigned long long>(ts.frames), static_cast<unsigned long long>(ts.matched),
                          static_cast<unsigned long long>(ts.beganBefore), static_cast<unsigned long long>(ts.endedAfter),
                          static_cast<double>(ts.startGapSum) / compared, static_cast<unsigned long long>(ts.startGapMax),
                          static_cast<double>(ts.endGapSum) / compared, static_cast<unsigned long long>(ts.endGapMax),
                          static_cast<unsigned long long>(ts.countDiffers),
                          static_cast<unsigned long long>(ts.framesWithoutGame),
                          static_cast<unsigned long long>(ts.passesWithoutGame));
            if (tr.scene)
                simqvfr::logf("lmu mvr: pass tracker, MSAA off: %llu passes begun on the scene's single-sample target, "
                              "%llu post-processings begun at YEBIS's copy (%llu at a resolve), %llu final blits seen "
                              "(%llu blits before a post not taken); %llu passes ended before their post; the target "
                              "learned again %llu times",
                              static_cast<unsigned long long>(ts.singleSamplePasses),
                              static_cast<unsigned long long>(s.singleSamplePosts),
                              static_cast<unsigned long long>(s.resolves),
                              static_cast<unsigned long long>(ts.singleSampleBlits),
                              static_cast<unsigned long long>(ts.blitsBeforePost),
                              static_cast<unsigned long long>(ts.passesWithoutPost),
                              static_cast<unsigned long long>(ts.sceneRelearned));
        }
        if (s.listsInPass || s.listsOutside)
            simqvfr::logf("lmu mvr: the game's command lists: %llu executed in a pass (%llu with the two views on as they "
                          "began), %llu outside; %llu hooked calls made while they executed; executed %s",
                          static_cast<unsigned long long>(s.listsInPass), static_cast<unsigned long long>(s.listsTwoViews),
                          static_cast<unsigned long long>(s.listsOutside), static_cast<unsigned long long>(s.callsInLists),
                          g.listsAsBefore.load(std::memory_order_relaxed)
                              ? "AS BEFORE THE FIX (the two views as they were, their calls handled as the game's)"
                              : "plain (the two views off, their calls passed on)");
        if (g.instanced)
            simqvfr::logf("lmu mvr: instancing: %llu draws made with twice their instances (%llu with a per-instance "
                          "layout's twin), %llu drawn into the eye alone (DrawAuto, indirect), %llu for their input "
                          "layout%s",
                          static_cast<unsigned long long>(s.doubledDraws), static_cast<unsigned long long>(s.layoutSwaps),
                          static_cast<unsigned long long>(s.eyeOnlyDraws), static_cast<unsigned long long>(s.layoutEyeOnly),
                          s.instancedTakeovers ? "; took over from NVIDIA's MVR, which the driver refused" : "");
        if (g.mask.on || s.focusMaskDraws)
            simqvfr::logf("lmu mvr: focus mask: drawn in %llu eye passes (recorded %llu times), left out of %llu for the "
                          "A/B, %llu for an unknown depth test, %llu with nothing to mask",
                          static_cast<unsigned long long>(s.focusMaskDraws),
                          static_cast<unsigned long long>(s.focusMaskRecords),
                          static_cast<unsigned long long>(s.focusMaskAbSkipped),
                          static_cast<unsigned long long>(s.focusMaskOtherTest),
                          static_cast<unsigned long long>(s.focusMaskEmpty));
        if (g.corridor.on || s.corridorDraws || s.corridorCopies)
            simqvfr::logf("lmu mvr: corridor: drawn in %llu eye passes, %llu copies of the eye's depth, left out of %llu "
                          "for the A/B, %llu without its input (no copy yet, no depth terms or an unknown depth test), "
                          "%llu with the world's up unknown (level), %llu tinted (show mode), %llu road passes, %llu "
                          "fills (%llu refused)",
                          static_cast<unsigned long long>(s.corridorDraws),
                          static_cast<unsigned long long>(s.corridorCopies),
                          static_cast<unsigned long long>(s.corridorAbSkipped),
                          static_cast<unsigned long long>(s.corridorNoInput),
                          static_cast<unsigned long long>(s.corridorLevelUp),
                          static_cast<unsigned long long>(s.corridorTints),
                          static_cast<unsigned long long>(s.corridorGrounds),
                          static_cast<unsigned long long>(s.corridorFills),
                          static_cast<unsigned long long>(s.corridorFillRefused));
        if (g.corridor.bands) {
            auto& c = g.corridor;
            simqvfr::logf("lmu mvr: corridor DLSS band: %llu focus views over %d%% of their rows; the corridor's rows (with "
                          "%d%% about them) %.0f%% of the view on average, taller than the band in %llu, none in view in %llu",
                          static_cast<unsigned long long>(c.bands), c.bandPct, c.bandPadPct,
                          c.bands > c.bandsNoRows ? 100.0 * c.bandNeeded / static_cast<double>(c.bands - c.bandsNoRows) : 0.0,
                          static_cast<unsigned long long>(c.bandsClipped), static_cast<unsigned long long>(c.bandsNoRows));
            c.bands = c.bandsClipped = c.bandsNoRows = 0;
            c.bandNeeded = 0;
        }
        if (s.maskDraws || s.maskDropped || s.maskAbDropped || s.panelReplays || s.uiMipFrames)
            simqvfr::logf("lmu mvr: lens mask: %llu draws (%llu with two views), %llu dropped while stale, %llu on the "
                          "A/B's side without it; VR UI panel draws replayed at the menus' native size: %llu; frames whose "
                          "panel sampled its canvas with mips: %llu",
                          static_cast<unsigned long long>(s.maskDraws), static_cast<unsigned long long>(s.maskDrawsTwo),
                          static_cast<unsigned long long>(s.maskDropped),
                          static_cast<unsigned long long>(s.maskAbDropped),
                          static_cast<unsigned long long>(s.panelReplays),
                          static_cast<unsigned long long>(s.uiMipFrames));
        // The viewport reads while ours were bound: the game's exe's, another DLL's, and how they were answered.
        simqvfr::logf("lmu mvr: viewport reads while ours were bound: %llu answered with the game's own; %llu by another "
                      "DLL than the game's exe (lmu_viewport_reads=%d: %s)",
                      static_cast<unsigned long long>(s.gameViewportReads),
                      static_cast<unsigned long long>(s.otherViewportReads), g.viewportReads,
                      g.viewportReads == 0   ? "not hooked"
                      : g.viewportReads == 2 ? "only the game's exe given its own"
                                             : "every caller but Sim-QVFR given the game's own");
        if (g.focusReplay) {
            const MvrProbeStats probe = mvr_probe_stats();
            simqvfr::logf("lmu mvr: focus post: %llu copies, %llu composites (%llu by their bindings), %llu final blits; "
                          "overlays doubled: %llu draws in the passes, %llu after them; %llu steps failed; %llu passes "
                          "without the composite after the copy, %llu draws into the resolve's target by another shader "
                          "(YEBIS's copy and composite pixel shaders marked %llu and %llu times); past a stage before "
                          "YEBIS: %llu copies from its image, %llu from the resolve; %llu scene rebinds kept in their pass",
                          static_cast<unsigned long long>(s.focusCopies),
                          static_cast<unsigned long long>(s.focusComposites),
                          static_cast<unsigned long long>(s.focusCompositesByBindings),
                          static_cast<unsigned long long>(s.focusBlits),
                          static_cast<unsigned long long>(s.overlayDraws),
                          static_cast<unsigned long long>(s.overlaysOutside),
                          static_cast<unsigned long long>(s.focusFailures),
                          static_cast<unsigned long long>(s.focusNoComposite),
                          static_cast<unsigned long long>(s.focusUnknownComposites),
                          static_cast<unsigned long long>(probe.yebisCopies),
                          static_cast<unsigned long long>(probe.yebisComposites),
                          static_cast<unsigned long long>(s.focusStageCopies),
                          static_cast<unsigned long long>(s.focusStageResolves),
                          static_cast<unsigned long long>(tr.stats.sceneRebinds));
        }
        for (int m = 0; m < 2; ++m) {
            if (!g.gpuCounts[m][0])
                continue;
            const double* sc = g.sceneMs[m];
            const double* po = g.postMs[m];
            const unsigned* n = g.gpuCounts[m];
            const auto avg = [](double sum, unsigned k) { return k ? sum / k : 0.0; };
            double total = 0;
            for (int p = 0; p < 4; ++p)
                total += avg(sc[p] + po[p], n[p]);
            simqvfr::logf("lmu mvr: GPU per pass%s, scene / post-processing (to the resolve / after it): pass 0 %.2f / %.2f, "
                          "pass 1 %.2f / %.2f, pass 2 %.2f / %.2f, pass 3 %.2f / %.2f ms, %.2f in all (%u frames timed)",
                          m ? " on the A/B's other side (without MVR, the four passes, instancing, without the other "
                              "cars' motion, or without the lens mask)"
                            : "", avg(sc[0], n[0]), avg(po[0], n[0]), avg(sc[1], n[1]), avg(po[1], n[1]),
                          avg(sc[2], n[2]), avg(po[2], n[2]), avg(sc[3], n[3]), avg(po[3], n[3]), total, n[0]);
            for (int p = 0; p < 4; ++p) {
                g.sceneMs[m][p] = g.postMs[m][p] = 0;
                g.gpuCounts[m][p] = 0;
            }
            // The focus corridor's own work on this side, per eye pass (the scene's time above includes it).
            const unsigned (&cn)[2][4] = g.corridorCounts[m];
            if (cn[0][0] || cn[1][0] || cn[0][2] || cn[1][2]) {
                const double (&cm)[2][4] = g.corridorMs[m];
                const auto cavg = [&](int p, int k) { return cn[p][k] ? cm[p][k] / cn[p][k] : 0.0; };
                simqvfr::logf("lmu mvr: corridor GPU%s: pass 0 depth copy %.3f, road pass %.3f, mask %.3f, fill %.3f ms; pass 1 "
                              "%.3f, %.3f, %.3f, %.3f ms; %.3f ms a frame in all (%u / %u frames with the copy, %u / %u "
                              "with the mask, %u / %u with the fill)",
                              m ? " on the A/B's other side" : "", cavg(0, 0), cavg(0, 1), cavg(0, 2), cavg(0, 3),
                              cavg(1, 0), cavg(1, 1), cavg(1, 2), cavg(1, 3),
                              cavg(0, 0) + cavg(0, 1) + cavg(0, 2) + cavg(0, 3) + cavg(1, 0) + cavg(1, 1) + cavg(1, 2) +
                                  cavg(1, 3),
                              cn[0][0], cn[1][0], cn[0][2], cn[1][2], cn[0][3], cn[1][3]);
                const unsigned (&fn)[2][5] = g.fillCounts[m];
                const double (&fm)[2][5] = g.fillMs[m];
                const auto favg = [&](int p, int k) { return fn[p][k] ? fm[p][k] / fn[p][k] : 0.0; };
                if (fn[0][1] || fn[1][1])
                    simqvfr::logf("lmu mvr: corridor fill GPU%s: pass 0 list entry %.3f, colour copy %.3f, outside draw "
                                  "%.3f, edge draw %.3f, list exit %.3f ms; pass 1 %.3f, %.3f, %.3f, %.3f, %.3f ms (%u / %u "
                                  "frames)",
                                  m ? " on the A/B's other side" : "", favg(0, 0), favg(0, 1), favg(0, 2), favg(0, 3),
                                  favg(0, 4), favg(1, 0), favg(1, 1), favg(1, 2), favg(1, 3), favg(1, 4), fn[0][1], fn[1][1]);
                for (auto& pass : g.fillMs[m])
                    for (double& v : pass)
                        v = 0;
                for (auto& pass : g.fillCounts[m])
                    for (unsigned& v : pass)
                        v = 0;
                for (auto& pass : g.corridorMs[m])
                    for (double& v : pass)
                        v = 0;
                for (auto& pass : g.corridorCounts[m])
                    for (unsigned& v : pass)
                        v = 0;
            }
        }
    }
}

void mvr_set_two_views(TwoViewsFn fn) { g.twoViews = fn ? fn : &nvapi_two_views; }

void mvr_use_instanced(bool on, bool fallback) {
    g.instanced = on;
    g.canInstance = on || fallback;
    if (!on)
        g.stereo = false;  // the next update() switches single-pass stereo as needed
    bool missing = false;  // an entry now wanted with none of its functions hooked (installed before it was wanted)
    for (int h = 0; h < kHooks; ++h)
        missing = missing || (wanted(h) && functions[h].count == 0);
    if (g.context && g.pass < 0 && missing)  // the draws it needs hooked in every mode of d3d11's (between passes)
        hook_every_mode();
}

bool mvr_instanced() { return g.instanced; }

void mvr_frame_mode(int mode) { g.mode = mode ? 1 : 0; }

void mvr_twins_changed() { ++g.twinEpoch; }

void mvr_time_hooks(bool on) {
    if (on && !timeHooks) {  // the timestamp reads' own share of a sampled section: the least of many empty ones
        uint64_t least = ~uint64_t{0};
        for (int i = 0; i < 4096; ++i) {
            const uint64_t t0 = __rdtsc();
            least = (std::min)(least, __rdtsc() - t0);
        }
        timedEmptyTicks = least;
    }
    timeHooksOn = on;
    if (!on)
        timeHooks = false;  // (on: from the next 1-in-16 frame)
}

MvrPassStats mvr_pass_stats() { return g.stats; }

CarMotionStats mvr_car_stats() { return g.cars.stats(); }

void mvr_car_light(bool on) {
    if (!g.context)  // before the install only (with the car slots: they say which pixels are cars')
        g.carLight = on;
}

void mvr_car_light_frame(const simqvfr::CarLighting& settings) {
    g.carLightSettings = settings;
    g.carLightOn = g.carLight && settings.on && g.slotsOn && g.carActive;
}

bool mvr_gpu_frame_ms(double& ms) { ms = g.gpuFrameMs.exchange(0, std::memory_order_relaxed); return ms > 0; }

bool mvr_car_light_costs(double& gpuMs, double& cpuMs) {
    const float gpu = carLightGpu.load(std::memory_order_relaxed), cpu = carLightCpu.load(std::memory_order_relaxed);
    if (!g.carLight || std::isnan(gpu) || std::isnan(cpu))
        return false;
    gpuMs = gpu;
    cpuMs = cpu;
    return true;
}

void mvr_car_light_block(unsigned serial) { g.carLightPass.set_block(serial); }

bool mvr_car_light_block_cost(unsigned serial, CarLightBlock& out) { return g.carLightPass.block(serial, out); }

bool mvr_broken() { return g.context && g.broken; }

void mvr_track_drive(TrackFn begin, TrackFn end) {
    tr.driveBegin = begin;
    tr.driveEnd = end;
}

void mvr_track_passes(bool on) {
    tr.on = on;
    tr.open = false;
    tr.passes = tr.actual = 0;
    if (!on) {  // off: the single-sample scene's target forgotten (learned again in a frame when on)
        tr.scene = nullptr;
        tr.sceneDesc = {};
        tr.stageCandidate = nullptr;
        tr.offScene = false;
    }
    tr.stats.lastFrameDraws = tr.draws;
    tr.draws = 0;
}

MvrTrackStats mvr_track_stats() { return tr.stats; }

bool mvr_single_sample_passes() { return tr.on && tr.scene && !tr.msaaSinceScene; }
