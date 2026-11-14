#include "adapter.h"

#include <windows.h>

#include "../../log.h"
#include "../identity_adapter.h"
#include "../lmu/mvr_probe.h"
#include "game.h"
#include "pass_probe.h"

namespace ats {
namespace {

// Identity only: the layer logs and QVFR doesn't start. Two log-only probes on the game's D3D11 device, each on unless
// its [experiment] key is 0:
//   - ats_pass_probe: the eye passes found from the render-target binds, their CPU time and draws (pass_probe.h);
//   - ats_shader_probe: every vertex shader converted to draw a second view and created through NVAPI (gate MV2 of the
//     multi-view playbook), then released. LMU's probe for now (its log lines say "lmu mvr probe"); it moves to
//     src/engine when ATS needs more of it than counting.
class AtsAdapter final : public simqvfr::games::IdentityAdapter {
public:
    AtsAdapter() : IdentityAdapter(&kGame) {}

    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        passProbe_ = analysis() && ini && GetPrivateProfileIntW(L"experiment", L"ats_pass_probe", 1, ini) != 0;
        shaderProbe_ = analysis() && ini && GetPrivateProfileIntW(L"experiment", L"ats_shader_probe", 1, ini) != 0;
        if (shaderProbe_)
            shaderProbe_ = lmu::mvr_probe_install(ini);
        simqvfr::logf("ats: identity only, log-only probes: pass %s, shader %s", passProbe_ ? "on" : "off",
                      shaderProbe_ ? "on" : "off");
    }
    bool observes_d3d11() const override { return passProbe_ || shaderProbe_; }
    void d3d11_session(ID3D11Device* device) override {
        if (shaderProbe_)
            lmu::mvr_probe_device(device);
        if (passProbe_)
            pass_probe_install(device);
    }
    void session_destroyed() override { pass_probe_session_end(); }
    void frame_end() override {
        if (passProbe_)
            pass_probe_frame_end();
        if (shaderProbe_)
            lmu::mvr_probe_frame_end();
    }

private:
    bool passProbe_ = false, shaderProbe_ = false;
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static AtsAdapter a;
    return a;
}

}  // namespace ats
