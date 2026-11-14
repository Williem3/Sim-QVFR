#pragma once

namespace simqvfr {

// ACE's API path needs ACE's own single-pass stereo (both eyes drawn by one list: the focus replays take those lists).
// It is an engine flag, off by default and not read from the command line. Found by name in the running exe (its flag
// registration), confirmed a bool (registered as another known bool flag is), and set to true, before ACE makes its
// renderer. Nothing is written when anything does not match; the log says why. True when it is on.
bool ace_single_pass_stereo_on();

}  // namespace simqvfr
