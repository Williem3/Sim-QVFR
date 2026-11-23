// A diagnostic for development builds ([experiment] log_cpp_exceptions=1): the first C++ exceptions thrown anywhere in
// the game's process are logged with their class names and, for a std::exception, its message. For a game that aborts
// on an error of its own without saying which (ACE under RenderDoc, 2026-11-23).
#pragma once

namespace simqvfr {

void cpp_exceptions_log_start();

}  // namespace simqvfr
