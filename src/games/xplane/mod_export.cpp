// sim-qvfr-xplane.dll's one export (SIM_QVFR_XPLANE_AS_MOD): X-Plane's mod entry (mod_entry.cpp), with the core's
// services bound to the host table first (mod_api/core_shim.h).
#include "../../mod_api/core_shim.h"
#include "adapter.h"

extern "C" __declspec(dllexport) const SqvfrMod* sqvfr_mod_entry(const SqvfrHost* host) {
    if (!sqvfr_shim::bind(host))
        return nullptr;
    return xplane::mod_entry(host);
}
