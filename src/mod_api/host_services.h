// The core's services for game mods (mod_api.h SqvfrHost): the one table every mod gets, filled from the core's own
// log, ini, hooks and code patches, so a mod shares the process's single copy of each (one log file, one MinHook).
#pragma once

#include "mod_api.h"

namespace simqvfr::mod_api {

// Lives for the process.
const SqvfrHost& host_table();

}  // namespace simqvfr::mod_api
