// ACC's copies and focus gates (the integration playbook's stages 4 and 5; docs/recon-acc.md "Gates"). At a few
// frames, ACC's own image (all four views where its final pass put them, as the layer submits and composes them) is
// read back at xrEndFrame and each focus view compared with its eye (gate_compare.h):
//  - copies ([experiment] acc_copies=1): views 2-3 render with the eyes' cameras at the eyes' size (the layer lays them
//    out so: GameAdapter::qvfr_copies_layout), compared pixel for pixel. Any difference is per-view state a focus
//    view would get wrong (exposure, history, screen-space effects, culling).
//  - focus (acc_focus=1): each focus view (off-axis: the eye's camera) against its box's crop of its eye: linear
//    luminance in a grid of cells, focus / eye, edge ring and middle apart, by brightness; and its alignment.
// The samples start at Ctrl+Alt+G (gate_hotkey, default 1: pressed once in the cockpit, so the menus aren't sampled),
// or with gate_hotkey=0 at the first four-view frame; then gate_first four-view frames later and every gate_every,
// gate_samples in all. A verdict line after the last ("acc copies: done: ..."); exit_when_done=1 then asks ACC to close.
// Images in logs\captures for tools/focus_compare.py.
#pragma once

struct ID3D11Texture2D;

namespace acc {

struct GateRun {
    bool copies = false;
    bool focus = false;
    int first = 300, every = 300, count = 3;
    bool hotkey = true;
    bool exitWhenDone = false;
    // gate_ab_contact=1 (focus gate): the samples take turns at contact shadows off (views.h
    // ViewsFixes::contactShadowsOff) and on, each state's flips summed up at the end.
    bool abContact = false;
    bool on() const { return copies || focus; }
};
// [experiment] acc_copies / acc_focus, gate_first, gate_every, gate_samples, gate_hotkey, exit_when_done.
GateRun gate_run(const wchar_t* ini);

void gates_start(const GateRun& run);
bool gates_on();
// Every xrEndFrame, on ACC's render thread (the image's device's immediate context), before the layer submits: the
// game's image and each view's rect in it (x, y, width, height).
void gates_image(ID3D11Texture2D* image, const int (&rects)[4][4]);

}  // namespace acc
