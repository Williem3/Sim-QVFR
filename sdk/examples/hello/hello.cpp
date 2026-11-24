// The smallest Sim-QVFR game mod: it identifies the game, reads its own settings and logs, and leaves the game's
// rendering alone (has_qvfr answers 0, so the game runs as it would without Sim-QVFR). Start a real mod from here: see
// ../../AGENTS.md for what to fill in for each feature.
#include <cstdio>
#include <string>

#include "mod.hpp"

namespace {

class Hello final : public sqvfr::Mod {
public:
    explicit Hello(const SqvfrHost* host) : host_(host) {}

    // No QVFR of our own: the game renders as it would without Sim-QVFR.
    int32_t has_qvfr() override { return 0; }

    void instance_created(const wchar_t*, const wchar_t*) override {
        if (!sqvfr::setting_on(host_, "greet", true))
            return;
        const std::string mood = sqvfr::setting_choice(host_, "mood", "calm");
        char line[160];
        std::snprintf(line, sizeof(line), "hello mod: running in %s, feeling %s", host_->gameId, mood.c_str());
        host_->log(SQVFR_LOG_INFO, line);
    }

private:
    const SqvfrHost* host_;
};

}  // namespace

extern "C" __declspec(dllexport) const SqvfrMod* sqvfr_mod_entry(const SqvfrHost* host) {
    if (!host || host->abi != SQVFR_ABI_VERSION || !host->gameId)
        return nullptr;
    static Hello mod(host);
    // gameId must be the game the core asked for (host->gameId): this example serves any.
    static const SqvfrMod table = sqvfr::mod_table(mod, host->gameId);
    return &table;
}
