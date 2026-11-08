#include "adapter.h"

#include <windows.h>

#include "../identity_adapter.h"
#include "game.h"
#include "view_probe.h"

namespace lmu {
namespace {

// Identity only: the layer logs, QVFR doesn't start. The probes below change nothing the game renders, run only when
// their [experiment] key asks, and never in a launch the anti-cheat protects (adapter_for hands out the plain identity
// adapter there).
class LmuAdapter final : public simqvfr::games::IdentityAdapter {
public:
    LmuAdapter() : IdentityAdapter(&kGame) {}
    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        viewProbe_ = analysis() && GetPrivateProfileIntW(L"experiment", L"lmu_view_probe", 0, ini) != 0 &&
                     view_probe_start();
    }
    void frame_end() override {
        if (viewProbe_)
            view_probe_frame_end();
    }

private:
    bool viewProbe_ = false;
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static LmuAdapter a;
    return a;
}

}  // namespace lmu
