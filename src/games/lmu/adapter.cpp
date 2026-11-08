#include "adapter.h"

#include <windows.h>

#include "../identity_adapter.h"
#include "four_passes.h"
#include "game.h"
#include "view_probe.h"

namespace lmu {
namespace {

// Identity only: the layer logs, QVFR doesn't start. Its experiments run only when their [experiment] key asks, and
// never in a launch the anti-cheat protects (adapter_for hands out the plain identity adapter there): the view probe
// (log only) and the copies gate (four passes, docs/recon-lmu.md).
class LmuAdapter final : public simqvfr::games::IdentityAdapter {
public:
    LmuAdapter() : IdentityAdapter(&kGame) {}
    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        if (!analysis())
            return;
        viewProbe_ = GetPrivateProfileIntW(L"experiment", L"lmu_view_probe", 0, ini) != 0 && view_probe_start();
        copies_ = GetPrivateProfileIntW(L"experiment", L"lmu_copies", 0, ini) != 0 &&
                  four_passes_start_copies(GetPrivateProfileIntW(L"experiment", L"lmu_copies_hold_exposure", 1, ini) != 0);
    }
    void frame_end() override {
        if (viewProbe_)
            view_probe_frame_end();
        if (copies_)
            four_passes_frame_end();
    }

private:
    bool viewProbe_ = false;
    bool copies_ = false;
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static LmuAdapter a;
    return a;
}

}  // namespace lmu
