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
        // lmu_focus=1: the focus gate (focus views); lmu_copies=1: the copies gate. Both run the eye loop four times.
        const bool focus = GetPrivateProfileIntW(L"experiment", L"lmu_focus", 0, ini) != 0;
        const bool copies = GetPrivateProfileIntW(L"experiment", L"lmu_copies", 0, ini) != 0;
        FourPassesRun run;
        run.focus = focus;
        run.holdExposure = GetPrivateProfileIntW(L"experiment", L"lmu_copies_hold_exposure", 1, ini) != 0;
        run.fraction = GetPrivateProfileIntW(L"experiment", L"focus_percent", 30, ini) / 100.0;
        run.first = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_first", 300, ini));
        run.every = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_every", 300, ini));
        run.count = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_samples", 3, ini));
        run.warmup = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_warmup", 60, ini));
        run.exitWhenDone = GetPrivateProfileIntW(L"experiment", L"exit_when_done", 0, ini) != 0;
        copies_ = (focus || copies) && four_passes_start(run);
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
