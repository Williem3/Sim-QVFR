#pragma once

// RF2's scene passes in a frame: a "scene pass" is a stretch with the eye's scene target (Renderer's lock) bound with a
// depth view, numbered from 1 in the frame. The eye-pass rule below is the one measured on the first headset and game
// settings this was built on: passes 2, 3 and 5 draw the left eye, 6 and 8 the right one; the left eye is composed at the
// first scene resolve after pass 3 closes, the right one after pass 6. The other passes draw as the game made them.
// [experiment] rf2_census=1 (census.h) logs the frame's binds and resolves to check the rule elsewhere.

namespace simqvfr::games::rf2 {

struct PassState {
    bool fullSceneOpen = false;
    unsigned fullScenePassInFrame = 0;
    int pendingCompositeEye = -1;
    unsigned long long lastCompositeFrame[2]{~0ull, ~0ull};

    void note_full_scene_bind(bool full) {
        if (full && !fullSceneOpen) {
            fullSceneOpen = true;
            ++fullScenePassInFrame;
        } else if (!full) {
            if (fullSceneOpen) {
                if (fullScenePassInFrame == 3)
                    pendingCompositeEye = 0;
                else if (fullScenePassInFrame == 6)
                    pendingCompositeEye = 1;
            }
            fullSceneOpen = false;
        }
    }
    bool real_eye_pass() const {
        if (!fullSceneOpen)
            return false;
        const unsigned p = fullScenePassInFrame;
        return p == 2 || p == 3 || p == 5 || p == 6 || p == 8;
    }
    int eye_for_current_pass() const {
        const unsigned p = fullScenePassInFrame;
        return p == 2 || p == 3 || p == 5 ? 0 : p == 6 || p == 8 ? 1 : -1;
    }
    // The eye to compose at this scene resolve, once a frame each; -1 for none.
    int consume_composite_eye(unsigned long long frame) {
        const int eye = pendingCompositeEye;
        if (eye < 0 || eye > 1 || lastCompositeFrame[eye] == frame)
            return -1;
        pendingCompositeEye = -1;
        lastCompositeFrame[eye] = frame;
        return eye;
    }
    void frame_end() {
        fullSceneOpen = false;
        fullScenePassInFrame = 0;
    }
};

}  // namespace simqvfr::games::rf2
