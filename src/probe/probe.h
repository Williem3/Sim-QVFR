// Log-only hooks on ACE's VR functions: every hooked call still runs the original unchanged.
#pragma once

namespace simqvfr {

// Verifies the exe hash and anchors, then installs the probes. Safe to call more than once.
bool probe_start();
void probe_stop();
// Called by the OpenXR layer once per xrEndFrame to drive the per-frame sampling window.
void probe_frame_end();
// True while the periodic every-call sampling window is open.
bool probe_sequence_active();
// Calls `fn(this)` at every entry to the named probe target (before the original runs). One
// observer per target; fn must never throw. Returns false for an unknown name.
bool probe_set_observer(const char* name, void (*fn)(void* self));

}  // namespace simqvfr
