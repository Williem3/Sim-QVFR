#pragma once

namespace simqvfr {

// ACE's API path needs ACE's own single-pass stereo (both eyes drawn by one list: the focus replays take those lists).
// It is an engine flag, off by default and not read from the command line. Found by name in the running exe (its flag
// registration), confirmed a bool (registered as another known bool flag is), and set to true, before ACE makes its
// renderer. Nothing is written when anything does not match; the log says why. True when it is on.
bool ace_single_pass_stereo_on();

// Whether this process is a build of ACE the API path was made for (the exe's SHA-256 among game.h's builds): the
// focus replays bind ACE's captured root signatures with that build's layouts. Hashed once, at the first call (logged).
bool ace_known_build();
// Offline tests (a WARP device, not ACE): the answer ace_known_build gives, in place of the exe's hash.
void ace_known_build_for_tests(bool known);

}  // namespace simqvfr
