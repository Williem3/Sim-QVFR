// Eye gaze through OpenXR actions: head spaces, the reference-space hooks, the menu's controller profiles,
// the gaze action set, and the PollEvent and SyncActions hooks.
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

constexpr XrDuration kSceneLocateRecentNs = 1'000'000'000;

bool attachedAlone = false;  // gaze_attach_alone attached the layer's set by itself (under gaze.mutex)

}  // namespace

// True when the gaze is not taken from this xrLocateViews: it locates in a head-locked space while the
// game also locates in a scene space.
bool gaze_skips_space(XrSpace space, XrTime time) {
    std::lock_guard guard(headSpaces.mutex);
    const bool headLocked = std::find(headSpaces.view.begin(), headSpaces.view.end(), space) != headSpaces.view.end();
    if (!headLocked) {
        headSpaces.sceneLocated = time;
        return false;
    }
    const XrDuration since = time - headSpaces.sceneLocated;
    const bool skip = headSpaces.sceneLocated != 0 && since > -kSceneLocateRecentNs && since < kSceneLocateRecentNs;
    if (skip && !headSpaces.skipLogged) {
        headSpaces.skipLogged = true;
        log_line("gaze: the game also locates its views in a head-locked (VIEW) space; the gaze follows only its "
                 "scene-space calls");
    }
    return skip;
}

XrResult XRAPI_CALL layerCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    const XrResult r = next.CreateReferenceSpace(session, info, space);
    if (XR_SUCCEEDED(r) && info && space && info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) {
        std::lock_guard guard(headSpaces.mutex);
        headSpaces.view.push_back(*space);
    }
    return r;
}

XrResult XRAPI_CALL layerDestroySpace(XrSpace space) {
    {
        std::lock_guard guard(headSpaces.mutex);
        std::erase(headSpaces.view, space);
    }
    return next.DestroySpace(space);
}

XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
    // v + 2w(u x v) + 2u x (u x v), u = q.xyz
    const XrVector3f u{q.x, q.y, q.z};
    auto cross = [](const XrVector3f& a, const XrVector3f& b) {
        return XrVector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const XrVector3f t = cross(u, v);
    const XrVector3f t2{2 * t.x, 2 * t.y, 2 * t.z};
    const XrVector3f c = cross(u, t2);
    return {v.x + q.w * t2.x + c.x, v.y + q.w * t2.y + c.y, v.z + q.w * t2.z + c.z};
}

namespace {

// Controller profiles for the settings menu: left stick (or pad) click opens and closes it, the right
// stick (or pad) selects and steps. Core profiles only: one that needs an extension ACE does not
// enable (HP's) is refused. Pimax controllers report as Touch controllers.
struct MenuProfile {
    const char* profile;
    const char* toggle;
    const char* navigate;
};

constexpr MenuProfile kMenuProfiles[] = {
    {"/interaction_profiles/oculus/touch_controller", "/user/hand/left/input/thumbstick/click",
     "/user/hand/right/input/thumbstick"},
    {"/interaction_profiles/valve/index_controller", "/user/hand/left/input/thumbstick/click",
     "/user/hand/right/input/thumbstick"},
    {"/interaction_profiles/microsoft/motion_controller", "/user/hand/left/input/thumbstick/click",
     "/user/hand/right/input/thumbstick"},
    {"/interaction_profiles/htc/vive_controller", "/user/hand/left/input/trackpad/click",
     "/user/hand/right/input/trackpad"},
};

}  // namespace

// Gaze (and menu) actions. Also with gaze off at launch, so gaze can be turned on live.
void gaze_init(XrInstance instance) {
    {
        // Handles and suggestions belong to one instance. ACE creates a short-lived instance before its
        // real one, and an action from the destroyed instance in the real one's suggestion made the
        // Pimax runtime read freed memory in xrSuggestInteractionProfileBindings (start-up crash).
        std::lock_guard guard(gaze.mutex);
        gaze.instance = XR_NULL_HANDLE;
        gaze.set = XR_NULL_HANDLE;
        gaze.action = gaze.menuToggle = gaze.menuNavigate = XR_NULL_HANDLE;
        gaze.profile = gaze.pose = XR_NULL_PATH;
        gaze.gameAction = XR_NULL_HANDLE;
        gaze.gameSpace = XR_NULL_HANDLE;
        gaze.syncFailLogged = false;
        gaze.bindings.clear();
        gaze.profileSuggested.clear();
        gaze.suggested = gaze.attached = false;
        attachedAlone = false;
        gaze.appSyncs = 0;
        gaze.space = XR_NULL_HANDLE;
    }
    if ((!experiment.quad && !experiment.composing()) || !next.CreateActionSet)
        return;
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(setInfo.actionSetName, "simqvfr_gaze");
    strcpy_s(setInfo.localizedActionSetName, "Sim-QVFR gaze");
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(actionInfo.actionName, "gaze");
    strcpy_s(actionInfo.localizedActionName, "Gaze");
    std::lock_guard guard(gaze.mutex);
    gaze.instance = instance;
    XrResult r = next.StringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &gaze.profile);
    if (XR_SUCCEEDED(r))
        r = next.StringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &gaze.pose);
    if (XR_SUCCEEDED(r))
        r = next.CreateActionSet(instance, &setInfo, &gaze.set);
    if (XR_SUCCEEDED(r))
        r = next.CreateAction(gaze.set, &actionInfo, &gaze.action);
    if (XR_FAILED(r)) {
        logf("gaze: action setup failed (%d); focus box stays centred", r);
        gaze.set = XR_NULL_HANDLE;
        runtime_info(L"eye_tracking", L"0");
        return;
    }
    gaze.bindings[gaze.profile].push_back({gaze.action, gaze.pose});
    log_line("gaze: action set created");
    // The settings menu's controller actions; without them the menu still works from the keyboard.
    XrActionCreateInfo toggleInfo{XR_TYPE_ACTION_CREATE_INFO};
    toggleInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strcpy_s(toggleInfo.actionName, "settings_menu");
    strcpy_s(toggleInfo.localizedActionName, "Sim-QVFR settings menu");
    XrActionCreateInfo navigateInfo{XR_TYPE_ACTION_CREATE_INFO};
    navigateInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    strcpy_s(navigateInfo.actionName, "settings_navigate");
    strcpy_s(navigateInfo.localizedActionName, "Sim-QVFR settings navigate");
    if (XR_FAILED(next.CreateAction(gaze.set, &toggleInfo, &gaze.menuToggle)) ||
        XR_FAILED(next.CreateAction(gaze.set, &navigateInfo, &gaze.menuNavigate))) {
        log_line("settings menu: controller actions failed; keyboard only");
        gaze.menuToggle = gaze.menuNavigate = XR_NULL_HANDLE;
        return;
    }
    for (const MenuProfile& p : kMenuProfiles) {
        XrPath profile = XR_NULL_PATH, toggle = XR_NULL_PATH, navigate = XR_NULL_PATH;
        if (XR_SUCCEEDED(next.StringToPath(instance, p.profile, &profile)) &&
            XR_SUCCEEDED(next.StringToPath(instance, p.toggle, &toggle)) &&
            XR_SUCCEEDED(next.StringToPath(instance, p.navigate, &navigate))) {
            gaze.bindings[profile].push_back({gaze.menuToggle, toggle});
            gaze.bindings[profile].push_back({gaze.menuNavigate, navigate});
        }
    }
}

// A path's name for the log ("?" when the runtime cannot say).
std::string path_name(XrInstance instance, XrPath path) {
    char name[XR_MAX_PATH_LENGTH] = "?";
    uint32_t length = 0;
    if (next.PathToString)
        next.PathToString(instance, path, sizeof(name), &length, name);
    return name;
}

XrResult XRAPI_CALL layerSuggestInteractionProfileBindings(XrInstance instance,
                                                          const XrInteractionProfileSuggestedBinding* suggested) {
    std::lock_guard guard(gaze.mutex);
    if (suggested)
        logf("input: the game suggests %u binding(s) for %s", suggested->countSuggestedBindings,
             path_name(instance, suggested->interactionProfile).c_str());
    const auto own = suggested ? gaze.bindings.find(suggested->interactionProfile) : gaze.bindings.end();
    if (!gaze.set || own == gaze.bindings.end())
        return next.SuggestInteractionProfileBindings(instance, suggested);
    std::vector<XrActionSuggestedBinding> bindings(suggested->suggestedBindings,
                                                   suggested->suggestedBindings + suggested->countSuggestedBindings);
    if (suggested->interactionProfile == gaze.profile)
        for (const XrActionSuggestedBinding& b : bindings)
            if (b.binding == gaze.pose && b.action != gaze.action)
                gaze.gameAction = b.action;
    bindings.insert(bindings.end(), own->second.begin(), own->second.end());
    XrInteractionProfileSuggestedBinding merged = *suggested;
    merged.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    merged.suggestedBindings = bindings.data();
    XrResult r = next.SuggestInteractionProfileBindings(instance, &merged);
    logf("input: added %zu layer binding(s) to the game's %u, result=%d", own->second.size(),
         suggested->countSuggestedBindings, r);
    if (XR_FAILED(r))
        return next.SuggestInteractionProfileBindings(instance, suggested);
    gaze.profileSuggested[suggested->interactionProfile] = true;
    gaze.suggested |= suggested->interactionProfile == gaze.profile;
    return r;
}

namespace {

// The layer's own bindings for every profile the game suggested nothing for. Under gaze.mutex.
void suggest_own_bindings() {
    for (const auto& [profile, bindings] : gaze.bindings) {
        if (gaze.profileSuggested[profile])
            continue;
        XrInteractionProfileSuggestedBinding own{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        own.interactionProfile = profile;
        own.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        own.suggestedBindings = bindings.data();
        const XrResult r = next.SuggestInteractionProfileBindings(gaze.instance, &own);
        gaze.profileSuggested[profile] = XR_SUCCEEDED(r);
        if (profile == gaze.profile)
            gaze.suggested = XR_SUCCEEDED(r);
        logf("input: suggested the layer's own %zu binding(s) for %s, result=%d", bindings.size(),
             path_name(gaze.instance, profile).c_str(), r);
    }
}

}  // namespace

// A game with no OpenXR input of its own (GameAdapter::attaches_action_sets): the layer's set attached by itself, once
// per session, at xrBeginSession.
void gaze_attach_alone(XrSession session) {
    std::lock_guard guard(gaze.mutex);
    if (!gaze.set || gaze.attached || !next.AttachSessionActionSets)
        return;
    suggest_own_bindings();
    XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    info.countActionSets = 1;
    info.actionSets = &gaze.set;
    const XrResult r = next.AttachSessionActionSets(session, &info);
    logf("gaze: attached alone (the game uses no OpenXR input), result=%d", r);
    gaze.attached = attachedAlone = XR_SUCCEEDED(r);
}

XrResult XRAPI_CALL layerAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info) {
    std::lock_guard guard(gaze.mutex);
    if (attachedAlone) {  // the adapter said the game wouldn't: its own attach fails (one per session)
        const XrResult r = next.AttachSessionActionSets(session, info);
        logf("gaze: the game attaches %u action set(s) of its own after the layer attached its gaze alone: result=%d "
             "(GameAdapter::attaches_action_sets is wrong for this game)",
             info ? info->countActionSets : 0, r);
        return r;
    }
    if (!gaze.set || !info)
        return next.AttachSessionActionSets(session, info);
    // Profiles the game suggested nothing for: the layer's bindings alone.
    suggest_own_bindings();
    std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
    sets.push_back(gaze.set);
    XrSessionActionSetsAttachInfo merged = *info;
    merged.countActionSets = static_cast<uint32_t>(sets.size());
    merged.actionSets = sets.data();
    XrResult r = next.AttachSessionActionSets(session, &merged);
    logf("gaze: attached with the game's %u action set(s), result=%d", info->countActionSets, r);
    if (XR_SUCCEEDED(r)) {
        gaze.attached = true;
        return r;
    }
    return next.AttachSessionActionSets(session, info);
}

// Session state and interaction profile changes, for the log (actions are only active while the session is FOCUSED).
XrResult XRAPI_CALL layerPollEvent(XrInstance instance, XrEventDataBuffer* event) {
    const XrResult r = next.PollEvent(instance, event);
    if (r == XR_SUCCESS && event) {
        if (event->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            static const char* const kStates[] = {"UNKNOWN", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE",
                                                  "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
            const auto state = reinterpret_cast<const XrEventDataSessionStateChanged*>(event)->state;
            logf("session state: %s", state >= 0 && state < 9 ? kStates[state] : "?");
        } else if (event->type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            log_line("input: interaction profile changed");
        }
    }
    return r;
}

XrResult XRAPI_CALL layerSyncActions(XrSession session, const XrActionsSyncInfo* info) {
    std::lock_guard guard(gaze.mutex);
    if (!gaze.attached || !info)
        return next.SyncActions(session, info);
    ++gaze.appSyncs;
    if (!gaze.syncLogged) {  // what the game syncs: a subaction path other than null leaves that set's other actions out
        gaze.syncLogged = true;
        std::string list;
        for (uint32_t i = 0; i < info->countActiveActionSets; ++i) {
            const XrActiveActionSet& a = info->activeActionSets[i];
            char entry[64];
            std::snprintf(entry, sizeof(entry), "%s%p ", i ? ", " : "", static_cast<void*>(a.actionSet));
            list += entry;
            list += a.subactionPath == XR_NULL_PATH ? std::string("(all)") : path_name(gaze.instance, a.subactionPath);
        }
        logf("gaze: the game syncs %u action set(s): %s; next %p", info->countActiveActionSets, list.c_str(), info->next);
    }
    std::vector<XrActiveActionSet> sets(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
    sets.push_back({gaze.set, XR_NULL_PATH});
    XrActionsSyncInfo merged = *info;
    merged.countActiveActionSets = static_cast<uint32_t>(sets.size());
    merged.activeActionSets = sets.data();
    const XrResult r = next.SyncActions(session, &merged);
    if (XR_FAILED(r) && !gaze.syncFailLogged) {
        gaze.syncFailLogged = true;
        logf("gaze: the game's xrSyncActions with the layer's set failed (%d; the game's %u set(s)); synced without it",
             r, info->countActiveActionSets);
    }
    return XR_SUCCEEDED(r) ? r : next.SyncActions(session, info);
}

}  // namespace simqvfr::layer
