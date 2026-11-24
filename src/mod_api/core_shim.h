// The core's services inside a first-party mod's DLL (core_shim.cpp): bound once, by the mod's entry, to the host
// table the core passed it.
#pragma once

#include "mod_api.h"

namespace sqvfr_shim {

// False: no table, another ABI, or a core whose table is shorter than this mod needs (the mod then refuses the core).
bool bind(const SqvfrHost* host);

}  // namespace sqvfr_shim
