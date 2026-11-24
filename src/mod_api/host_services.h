// The core's services for game mods (mod_api.h SqvfrHost): the one table every mod gets, filled from the core's own
// log, ini, hooks and code patches, so a mod shares the process's single copy of each (one log file, one MinHook).
#pragma once

#include "mod_api.h"

namespace simqvfr::mod_api {

// Lives for the process.
const SqvfrHost& host_table();

// The ini mods' approvals and settings are in: SIM_QVFR_INI (the launcher sets it), else sim-qvfr.ini beside the core.
std::wstring mods_ini();

}  // namespace simqvfr::mod_api
