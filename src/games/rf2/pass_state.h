#pragma once
namespace simqvfr::games::rf2 {
struct PassState {
    bool fullSceneOpen = false;
    unsigned fullScenePassInFrame = 0;
    int pendingCompositeEye = -1;
    unsigned long long lastCompositeFrame[2]{~0ull, ~0ull};
    void note_full_scene_bind(bool full) {
        if (full && !fullSceneOpen) { fullSceneOpen = true; ++fullScenePassInFrame; }
        else if (!full) {
            if (fullSceneOpen) {
                if (fullScenePassInFrame == 3) pendingCompositeEye = 0;
                else if (fullScenePassInFrame == 6) pendingCompositeEye = 1;
            }
            fullSceneOpen = false;
        }
    }
    bool real_eye_pass() const {
        if (!fullSceneOpen) return false;
        const unsigned p = fullScenePassInFrame;
        return p == 2 || p == 3 || p == 5 || p == 6 || p == 8;
    }
    int eye_for_current_pass() const {
        const unsigned p = fullScenePassInFrame;
        return p == 2 || p == 3 || p == 5 ? 0 : p == 6 || p == 8 ? 1 : -1;
    }
    int consume_composite_eye(unsigned long long frame) {
        const int eye = pendingCompositeEye;
        if (eye < 0 || eye > 1 || lastCompositeFrame[eye] == frame) return -1;
        pendingCompositeEye = -1;
        lastCompositeFrame[eye] = frame;
        return eye;
    }
    void frame_end() { fullSceneOpen = false; fullScenePassInFrame = 0; }
};
}
