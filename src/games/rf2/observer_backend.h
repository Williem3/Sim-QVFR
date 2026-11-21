#pragma once

#include <algorithm>
#include <climits>
#include <iterator>
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

#include "../../log.h"

namespace simqvfr::games::rf2 {

// Which tessellation/geometry/stream-out stages the game has bound (the hook layer tracks them).
struct StageBindings {
    bool gs = false, hs = false, ds = false, so = false;
};

// Active RF2 scene observer. It records the verified 796x774 eye-pass sequence and
// shader-twin coverage without changing render state or owning D3D resources.
// "Candidate" = a bind of the full-size scene target with a 796x774 viewport and depth.
struct Observer {
    unsigned long long frame = 0;
    bool candidate796 = false;
    unsigned candidateInFrame = 0;
    uint64_t stageDraws = 0, stageEligible = 0, stageGS = 0, stageHS = 0, stageDS = 0, stageSO = 0;
    uint64_t stageTwin = 0, stageNoTwin = 0;
    uint64_t candidateDraws = 0, candidateResolves = 0;
    uint64_t framesObserved = 0, frames0 = 0, frames1 = 0, frames2 = 0, framesMore = 0;
    uint64_t candidateDrawSum[2]{}, candidateDrawMin[2]{UINT64_MAX, UINT64_MAX}, candidateDrawMax[2]{};
    uint64_t candidateResolveSum[2]{};
    uint64_t candidateShaderDraws[2]{}, candidateTwinDraws[2]{}, candidateNoTwinDraws[2]{};

    // Verified RF2 full-size eye scene: 3184x3096 f10 x4 + depth. Pass 3 = left, pass 6 = right.
    float fovRemap[2][4] = {{3.3333333f, 0.0f, 3.3333333f, 0.0f},
                            {3.3333333f, 0.0f, 3.3333333f, 0.0f}};
    float focusRect[2][4] = {{0.35f, 0.35f, 0.65f, 0.65f},
                             {0.35f, 0.35f, 0.65f, 0.65f}};

    // Index (0/1) of the current candidate within the frame, or UINT_MAX beyond the second.
    unsigned candidate_index() const {
        return candidateInFrame >= 1 && candidateInFrame <= 2 ? candidateInFrame - 1 : UINT_MAX;
    }

    void end_candidate() {
        if (!candidate796) return;
        const unsigned index = candidate_index();
        if (index < 2) {
            candidateDrawSum[index] += candidateDraws;
            candidateDrawMin[index] = std::min(candidateDrawMin[index], candidateDraws);
            candidateDrawMax[index] = std::max(candidateDrawMax[index], candidateDraws);
            candidateResolveSum[index] += candidateResolves;
        }
        candidate796 = false;
    }

    // A render-target bind on the tracked context. `scene`: slot 0 is the full-size scene target.
    void note_target_bind(ID3D11DeviceContext* context, bool scene, bool hasDepth) {
        if (!scene) {
            end_candidate();
            return;
        }
        D3D11_VIEWPORT vp[16]{};
        UINT count = static_cast<UINT>(std::size(vp));
        context->RSGetViewports(&count, vp);
        const bool is796 = count && vp[0].Width == 796.0f && vp[0].Height == 774.0f && hasDepth;
        if (is796 && !candidate796) {
            candidate796 = true;
            ++candidateInFrame;
            candidateDraws = 0;
            candidateResolves = 0;
        }
    }

    // A draw on the tracked context. `hasTwin(vs)`: the hook layer's twin lookup for the way the passes draw.
    template <class HasTwin>
    void note_draw(ID3D11DeviceContext* context, const StageBindings& stage, const GUID& twinGuid,
                   HasTwin&& hasTwin) {
        if (!candidate796) return;
        ++candidateDraws;
        ++stageDraws;
        stageGS += stage.gs ? 1 : 0;
        stageHS += stage.hs ? 1 : 0;
        stageDS += stage.ds ? 1 : 0;
        stageSO += stage.so ? 1 : 0;
        stageEligible += !stage.gs && !stage.hs && !stage.ds && !stage.so ? 1 : 0;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
        context->VSGetShader(&vs, nullptr, nullptr);
        if (!vs) return;
        UINT twinBytes = 0;
        if (vs->GetPrivateData(twinGuid, &twinBytes, nullptr) == S_OK && twinBytes)
            ++stageTwin;
        else
            ++stageNoTwin;
        const unsigned index = candidate_index();
        if (index < 2) {
            ++candidateShaderDraws[index];
            if (hasTwin(vs.Get()))
                ++candidateTwinDraws[index];
            else
                ++candidateNoTwinDraws[index];
        }
    }

    // A resolve of the full-size scene on the tracked context.
    void note_scene_resolve() {
        if (candidate796) ++candidateResolves;
    }

    void frame_end(unsigned long long newFrame, uint64_t stereoErrors) {
        ++framesObserved;
        if (candidateInFrame == 0) ++frames0;
        else if (candidateInFrame == 1) ++frames1;
        else if (candidateInFrame == 2) ++frames2;
        else ++framesMore;

        frame = newFrame;
        if (newFrame > 0 && newFrame % 300 == 0) {
            const auto avg = [](uint64_t sum, uint64_t count) {
                return count ? static_cast<double>(sum) / count : 0.0;
            };
            const uint64_t c0 = frames2 + frames1 + framesMore;
            const uint64_t c1 = frames2 + framesMore;
            simqvfr::logf("rf2 observer: frames=%llu c0=%llu c1=%llu c2=%llu c>2=%llu; "
                          "eye0 draws min/avg/max=%llu/%.1f/%llu resolves=%llu; "
                          "eye1 draws min/avg/max=%llu/%.1f/%llu resolves=%llu; "
                          "twin coverage eye0=%llu/%llu noTwin=%llu eye1=%llu/%llu noTwin=%llu; "
                          "stereoErrors=%llu; stage draws=%llu eligible=%llu GS=%llu HS=%llu DS=%llu SO=%llu instTwin=%llu noTwin=%llu",
                          static_cast<unsigned long long>(framesObserved),
                          static_cast<unsigned long long>(frames0),
                          static_cast<unsigned long long>(frames1),
                          static_cast<unsigned long long>(frames2),
                          static_cast<unsigned long long>(framesMore),
                          static_cast<unsigned long long>(candidateDrawMin[0] == UINT64_MAX ? 0 : candidateDrawMin[0]),
                          avg(candidateDrawSum[0], c0),
                          static_cast<unsigned long long>(candidateDrawMax[0]),
                          static_cast<unsigned long long>(candidateResolveSum[0]),
                          static_cast<unsigned long long>(candidateDrawMin[1] == UINT64_MAX ? 0 : candidateDrawMin[1]),
                          avg(candidateDrawSum[1], c1),
                          static_cast<unsigned long long>(candidateDrawMax[1]),
                          static_cast<unsigned long long>(candidateResolveSum[1]),
                          static_cast<unsigned long long>(candidateTwinDraws[0]),
                          static_cast<unsigned long long>(candidateShaderDraws[0]),
                          static_cast<unsigned long long>(candidateNoTwinDraws[0]),
                          static_cast<unsigned long long>(candidateTwinDraws[1]),
                          static_cast<unsigned long long>(candidateShaderDraws[1]),
                          static_cast<unsigned long long>(candidateNoTwinDraws[1]),
                          static_cast<unsigned long long>(stereoErrors),
                          static_cast<unsigned long long>(stageDraws),
                          static_cast<unsigned long long>(stageEligible),
                          static_cast<unsigned long long>(stageGS),
                          static_cast<unsigned long long>(stageHS),
                          static_cast<unsigned long long>(stageDS),
                          static_cast<unsigned long long>(stageSO),
                          static_cast<unsigned long long>(stageTwin),
                          static_cast<unsigned long long>(stageNoTwin));
        }

        candidateInFrame = 0;
    }
};

}  // namespace simqvfr::games::rf2
