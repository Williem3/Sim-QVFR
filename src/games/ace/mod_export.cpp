// sim-qvfr-ace.dll's one export (SIM_QVFR_ACE_AS_MOD): ACE's mod entry (mod_entry.cpp), with the core's services
// bound to the host table first (mod_api/core_shim.h).
#include "../../mod_api/core_shim.h"
#include "adapter.h"

extern "C" __declspec(dllexport) const SqvfrMod* sqvfr_mod_entry(const SqvfrHost* host) {
    if (!sqvfr_shim::bind(host))
        return nullptr;
    return ace::mod_entry(host);
}
