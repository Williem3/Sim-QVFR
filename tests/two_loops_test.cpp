// Offline check of the two_loops frame pairing against a fake runtime that enforces OpenXR's frame
// rules (wait before begin, begin/end strictly alternating), for ACE's normal call order and for a
// frame generator above the layer (two ends per begin), which must never arm.
#include "layer/layer.h"
#include "../src/games/ace/game.h"

#include <cstdio>

namespace {

struct FakeRuntime {
    int waits = 0, begins = 0, ends = 0, violations = 0;
    bool waited = false, begun = false;
    XrTime time = 1000;
} rt;

XrResult XRAPI_CALL fakeWait(XrSession, const XrFrameWaitInfo*, XrFrameState* s) {
    ++rt.waits;
    rt.waited = true;
    s->predictedDisplayTime = rt.time += 11111;
    s->predictedDisplayPeriod = 11111;
    s->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeBegin(XrSession, const XrFrameBeginInfo*) {
    ++rt.begins;
    if (!rt.waited || rt.begun) ++rt.violations;
    rt.waited = false;
    rt.begun = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeEnd(XrSession, const XrFrameEndInfo* info) {
    ++rt.ends;
    if (!rt.begun) ++rt.violations;
    rt.begun = false;
    return info->displayTime == rt.time ? XR_SUCCESS : XR_ERROR_TIME_INVALID;
}

int run(int appFrames, bool frameGenerator) {
    using namespace simqvfr;
    using namespace simqvfr::layer;
    rt = FakeRuntime{};
    loops.active = loops.broken = false;
    loops.waits = loops.begins = loops.ends = 0;
    next.WaitFrame = fakeWait;
    next.BeginFrame = fakeBegin;
    next.EndFrame = fakeEnd;
    experiment.twoLoops = true;
    int errors = 0;
    for (int i = 0; i < appFrames; ++i) {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        layerWaitFrame(nullptr, nullptr, &state);
        layerBeginFrame(nullptr, nullptr);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = state.predictedDisplayTime;
        errors += XR_FAILED(layerEndFrame(nullptr, &end));
        if (frameGenerator)  // a generated frame submitted between app frames
            errors += XR_FAILED(layerEndFrame(nullptr, &end));
    }
    return errors;
}

// A layer swapchain under OpenXR's image rules (3 images): acquire within the count, wait only on an acquired image,
// release only a waited one; the first `timeoutsLeft` waits time out.
struct FakeImages {
    int acquiredNotWaited = 0, waitedNotReleased = 0;
    int acquires = 0, waits = 0, releases = 0, violations = 0, timeoutsLeft = 0;
    uint32_t nextIndex = 0;
} images;

XrResult XRAPI_CALL fakeAcquireImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    if (images.acquiredNotWaited + images.waitedNotReleased >= 3) {
        ++images.violations;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    ++images.acquires;
    ++images.acquiredNotWaited;
    *index = images.nextIndex++ % 3;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeWaitImage(XrSwapchain, const XrSwapchainImageWaitInfo* info) {
    ++images.waits;
    if (images.acquiredNotWaited == 0 || !info || info->timeout == XR_INFINITE_DURATION) {
        ++images.violations;  // no image to wait on, or an unbounded wait
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (images.timeoutsLeft > 0) {
        --images.timeoutsLeft;
        return XR_TIMEOUT_EXPIRED;
    }
    --images.acquiredNotWaited;
    ++images.waitedNotReleased;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeReleaseImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
    if (images.waitedNotReleased == 0) {
        ++images.violations;  // released before its wait completed
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    --images.waitedNotReleased;
    ++images.releases;
    return XR_SUCCESS;
}

std::vector<XrSwapchain> destroyedChains;
XrResult XRAPI_CALL fakeDestroyChain(XrSwapchain chain) {
    destroyedChains.push_back(chain);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeDestroySpaceCall(XrSpace) { return XR_SUCCESS; }

XrSwapchain handle(uintptr_t value) { return reinterpret_cast<XrSwapchain>(value); }

// A timed-out wait keeps its image: no second acquire, no release, the next call waits on the same image; never an
// unbounded wait.
bool image_wait_rules() {
    using namespace simqvfr::layer;
    images = FakeImages{};
    next.AcquireSwapchainImage = fakeAcquireImage;
    next.WaitSwapchainImage = fakeWaitImage;
    next.ReleaseSwapchainImage = fakeReleaseImage;
    const XrSwapchain chain = handle(0x700);
    PendingImage pending;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    images.timeoutsLeft = 2 * kImageWaitTries + 5;  // two whole calls time out, the third gets it
    uint32_t index = 99;
    const XrResult first = acquire_wait(chain, pending, kImageWaitNs, kImageWaitTries, index);
    const XrResult second = acquire_wait(chain, pending, kImageWaitNs, kImageWaitTries, index);
    const bool heldWhileTimedOut = first == XR_TIMEOUT_EXPIRED && second == XR_TIMEOUT_EXPIRED && pending.acquired &&
                                   images.acquires == 1 && images.releases == 0 && index == 99;
    const XrResult third = acquire_wait(chain, pending, kImageWaitNs, kImageWaitTries, index);
    const bool sameImage = third == XR_SUCCESS && index == 0 && !pending.acquired && images.acquires == 1;
    next.ReleaseSwapchainImage(chain, &release);
    const XrResult fourth = acquire_wait(chain, pending, kImageWaitNs, kImageWaitTries, index);  // the fast case
    const bool fast = fourth == XR_SUCCESS && index == 1 && images.acquires == 2;
    next.ReleaseSwapchainImage(chain, &release);
    // A single short wait (the menu's, the overlay's): timed out, then waited on again.
    images.timeoutsLeft = 1;
    const bool shortTimeout = acquire_wait(chain, pending, kImageWaitNs, 1, index) == XR_TIMEOUT_EXPIRED &&
                              acquire_wait(chain, pending, kImageWaitNs, 1, index) == XR_SUCCESS && index == 2 &&
                              images.acquires == 3;
    next.ReleaseSwapchainImage(chain, &release);
    std::printf("image waits: held while timed out=%d, then the same image=%d, fast case=%d, short timeout=%d, "
                "%d waits, %d rule violations\n",
                heldWhileTimedOut ? 1 : 0, sameImage ? 1 : 0, fast ? 1 : 0, shortTimeout ? 1 : 0, images.waits,
                images.violations);
    next.AcquireSwapchainImage = nullptr;
    next.WaitSwapchainImage = nullptr;
    next.ReleaseSwapchainImage = nullptr;
    return heldWhileTimedOut && sameImage && fast && shortTimeout && images.violations == 0;
}

// xrDestroySwapchain: the layer's per-swapchain state for it goes first, an adopted eye chain with its output chain,
// and the call goes on; another chain's state stays.
bool destroy_swapchain_purges() {
    using namespace simqvfr::layer;
    destroyedChains.clear();
    next.DestroySwapchain = fakeDestroyChain;
    const XrSwapchain gameChain = handle(0x801), other = handle(0x802), eyes = handle(0x803), out = handle(0x804);
    {
        std::lock_guard guard(chainMutex);
        chainSizes[gameChain] = {64, 64};
        chainSizes[other] = {32, 32};
    }
    {
        std::lock_guard guard(v4d3d11.mutex);
        v4d3d11.chains[gameChain].format = 29;
        v4d3d11.chains[other].format = 29;
    }
    {
        std::lock_guard guard(v4outline.mutex);
        v4outline.formats[gameChain] = 29;
        v4outline.acquired[gameChain] = 1;
        v4outline.chain = gameChain;
    }
    {
        std::lock_guard guard(quad.mutex);
        quad.appChain = eyes;
        quad.outChain = out;
        quad.outPending.acquired = true;
    }
    measuredEyeChain = gameChain;
    layerDestroySwapchain(gameChain);
    bool purged, kept;
    {
        std::lock_guard guard(chainMutex);
        purged = !chainSizes.count(gameChain);
        kept = chainSizes.count(other) == 1;
    }
    {
        std::lock_guard guard(v4d3d11.mutex);
        purged &= !v4d3d11.chains.count(gameChain);
        kept &= v4d3d11.chains.count(other) == 1;
    }
    {
        std::lock_guard guard(v4outline.mutex);
        purged &= !v4outline.formats.count(gameChain) && !v4outline.acquired.count(gameChain) && v4outline.chain == XR_NULL_HANDLE;
    }
    purged &= measuredEyeChain.load() == XR_NULL_HANDLE;
    bool quadKept;
    {
        std::lock_guard guard(quad.mutex);
        quadKept = quad.appChain == eyes && quad.outChain == out;
    }
    layerDestroySwapchain(eyes);  // the adopted eye chain: its output chain goes too, adoption starts again
    bool unadopted;
    {
        std::lock_guard guard(quad.mutex);
        unadopted = quad.appChain == XR_NULL_HANDLE && quad.outChain == XR_NULL_HANDLE && !quad.outPending.acquired;
    }
    const bool calledDown = destroyedChains == std::vector<XrSwapchain>{gameChain, out, eyes};
    std::printf("xrDestroySwapchain: its state purged=%d, another chain's kept=%d, quad chains kept for another=%d, "
                "adopted chain released with its output=%d, called down=%d\n",
                purged ? 1 : 0, kept ? 1 : 0, quadKept ? 1 : 0, unadopted ? 1 : 0, calledDown ? 1 : 0);
    {
        std::lock_guard guard(chainMutex);
        chainSizes.clear();
    }
    {
        std::lock_guard guard(v4d3d11.mutex);
        v4d3d11.chains.clear();
    }
    next.DestroySwapchain = nullptr;
    return purged && kept && quadKept && unadopted && calledDown;
}

// xrDestroySpace on the space the gaze thread locates against: it is cleared first; another space leaves it.
bool destroy_space_clears_gaze_base() {
    using namespace simqvfr::layer;
    next.DestroySpace = fakeDestroySpaceCall;
    const XrSpace base = reinterpret_cast<XrSpace>(static_cast<uintptr_t>(0x901));
    const XrSpace other = reinterpret_cast<XrSpace>(static_cast<uintptr_t>(0x902));
    {
        std::lock_guard guard(gazeThread.mutex);
        gazeThread.base = base;
        gazeThread.directionTime = 1234;
    }
    layerDestroySpace(other);
    bool keptForOther, cleared;
    {
        std::lock_guard guard(gazeThread.mutex);
        keptForOther = gazeThread.base == base && gazeThread.directionTime == 1234;
    }
    layerDestroySpace(base);
    {
        std::lock_guard guard(gazeThread.mutex);
        cleared = gazeThread.base == XR_NULL_HANDLE && gazeThread.directionTime == 0;
    }
    std::printf("xrDestroySpace: the gaze thread's space kept for another=%d, cleared for its own=%d\n",
                keptForOther ? 1 : 0, cleared ? 1 : 0);
    next.DestroySpace = nullptr;
    return keptForOther && cleared;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    simqvfr::layer::game = &simqvfr::games::adapter_for(&ace::kGame);  // views4 below is ACE's
    const int errors = run(130, false);
    std::printf("ACE order: runtime waits=%d begins=%d ends=%d violations=%d endErrors=%d active=%d\n", rt.waits,
                rt.begins, rt.ends, rt.violations, errors, simqvfr::layer::loops.active ? 1 : 0);
    // 30 unpaired frames, then 100 app frames paired into 50 display frames
    bool ok = rt.waits == 80 && rt.begins == 80 && rt.ends == 80 && rt.violations == 0 && errors == 0;

    run(130, true);
    std::printf("frame generator above: active=%d broken=%d (expected 0 and 1)\n", simqvfr::layer::loops.active ? 1 : 0,
                simqvfr::layer::loops.broken ? 1 : 0);
    ok &= !simqvfr::layer::loops.active && simqvfr::layer::loops.broken;

    // The layer's own swapchain images and the game's swapchains and spaces going.
    ok &= image_wait_rules();
    ok &= destroy_swapchain_purges();
    ok &= destroy_space_clears_gaze_base();

    // Live settings: an edited ini applies at the next xrEndFrame, but never between frames A and B.
    {
        using namespace simqvfr;
        using namespace simqvfr::layer;
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring ini = std::wstring(temp) + L"sim-qvfr-live-test.ini";
        WritePrivateProfileStringW(L"experiment", L"focus_percent", L"35", ini.c_str());
        live = Live{};
        live.ini = ini;  // as load_experiment leaves it: written time read at launch
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &attributes);
        live.written = attributes.ftLastWriteTime;
        experiment.focusFraction = 0.35;
        experiment.feather = 0.10;
        Sleep(20);
        WritePrivateProfileStringW(L"experiment", L"focus_percent", L"50", ini.c_str());
        WritePrivateProfileStringW(L"quad", L"feather_percent", L"25", ini.c_str());
        loops.active = true;
        loops.pairedEnds = 1;  // A ended, B pending
        live_reload();  // points the ini worker at the file
        ini_worker_sync();  // it has read the change (the render thread never reads the file)
        live_reload();
        const bool heldForB = experiment.focusFraction == 0.35;
        loops.pairedEnds = 2;
        live_reload();
        const bool applied = experiment.focusFraction == 0.5 && experiment.feather == 0.25;
        experiment.focusFraction = 0.35;
        ini_worker_sync();
        live_reload();  // unchanged file: nothing re-read
        const bool quiet = experiment.focusFraction == 0.35;
        loops.active = false;
        live = Live{};
        DeleteFileW(ini.c_str());
        std::printf("live reload: held between A and B=%d, applied after B=%d, no re-read=%d\n", heldForB ? 1 : 0,
                    applied ? 1 : 0, quiet ? 1 : 0);
        ok &= heldForB && applied && quiet;
    }

    // The settings menu opens (its first key press) and steps a value into the ini, without a session
    // or panel: the open path takes the layer's locks, so this would hang or throw on a lock taken twice.
    {
        using namespace simqvfr;
        using namespace simqvfr::layer;
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring ini = std::wstring(temp) + L"sim-qvfr-menu-open-test.ini";
        WritePrivateProfileStringW(L"experiment", L"periphery_scale", L"30", ini.c_str());
        live = Live{};
        live.ini = ini;
        experiment.views4 = true;
        experiment.views4Layout = views4_layout(0.3, 0.39);
        {
            std::lock_guard guard(quad.mutex);
            quad.nativeW = 5190;
            quad.nativeH = 4168;
        }
        const int64_t now = qpc_now();
        menu_press(MenuKey::Right, now);  // opens
        const bool opened = menu.visible && menu.model.launch.nativeWidth == 5190 && menu.model.values.periphery == 30;
        // Past the page switch (and the Profile line, when there are profiles) to periphery.
        for (int i = 0; i < 8 && menu_page_items(menu.model)[menu.model.selected] != MenuItem::Periphery; ++i)
            menu_press(MenuKey::Down, now);
        menu_press(MenuKey::Left, now);  // periphery 30 -> 25
        ini_worker_sync();  // written on the ini worker
        const bool stepped = GetPrivateProfileIntW(L"experiment", L"periphery_scale", 0, ini.c_str()) == 25;
        DWORD writer = 0;
        ini_write_later([&writer] { writer = GetCurrentThreadId(); });
        ini_worker_sync();
        const bool offThread = writer != 0 && writer != GetCurrentThreadId();
        std::printf("ini writes: on the ini worker (not the render thread)=%d\n", offThread ? 1 : 0);
        ok &= offThread;
        // Two steps in quick succession while frames run (live_reload each frame): both reach the ini, and the
        // second is applied too (the menu then shows it), not only the first.
        live_reload();
        ini_worker_sync();
        live_reload();
        menu_press(MenuKey::Left, qpc_now());  // 25 -> 20
        for (int i = 0; i < 3; ++i) {
            Sleep(11);
            live_reload();
        }
        menu_press(MenuKey::Left, qpc_now());  // 20 -> 15
        bool second = false;
        for (int i = 0; i < 200 && !second; ++i) {
            Sleep(11);
            live_reload();
            second = menu.model.values.periphery == 15;
        }
        std::printf("two quick steps: the ini has %d, the menu shows %d after the reloads\n",
                    GetPrivateProfileIntW(L"experiment", L"periphery_scale", 0, ini.c_str()), menu.model.values.periphery);
        ok &= second;
        // A reload read before the latest step was written leaves the menu as it is; one read after it shows.
        IniSnapshot older;
        older.menu = menu.model.values;
        older.menu.periphery = 40;
        older.cheeky = menu.model.cheeky.values;
        older.writesBefore = menu.lastWrite - 1;
        menu_values_changed(older);
        const bool keptStep = menu.model.values.periphery == 15;
        older.writesBefore = menu.lastWrite;
        menu_values_changed(older);
        const bool newerShown = menu.model.values.periphery == 40;
        std::printf("menu reloads: one older than the last step ignored=%d, a newer one shown=%d\n", keptStep ? 1 : 0,
                    newerShown ? 1 : 0);
        ok &= keptStep && newerShown;
        // Steps quicker than the worker are written as one: while a write holds the worker, steps queued one behind
        // another become the newest; a write of another kind between them keeps its place, and the numbers stay in
        // order (a merged step's number counts as done).
        {
            HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ini_write_later([gate] { WaitForSingleObject(gate, 5000); });
            Sleep(30);
            int steps = 0, other = 0;
            ini_write_later([&steps] { steps += 1; }, IniWrite::MenuStep);
            ini_write_later([&steps] { steps += 10; }, IniWrite::MenuStep);
            ini_write_later([&other] { ++other; });
            const uint64_t last = ini_write_later([&steps] { steps += 100; }, IniWrite::MenuStep);
            SetEvent(gate);
            ini_worker_sync();
            uint64_t done = 0;
            {
                std::lock_guard guard(iniWorker.mutex);
                done = iniWorker.writesDone;
            }
            std::printf("merged menu steps: ran %d (want 110), other write %d, done through %llu of %llu\n", steps, other,
                        static_cast<unsigned long long>(done), static_cast<unsigned long long>(last));
            ok &= steps == 110 && other == 1 && done == last;
            // The menu itself: three steps while the worker is held reach the ini as the last one, and the menu
            // shows it after the reloads.
            ResetEvent(gate);
            ini_write_later([gate] { WaitForSingleObject(gate, 5000); });
            Sleep(30);
            menu_press(MenuKey::Left, qpc_now());  // 40 -> 35
            menu_press(MenuKey::Left, qpc_now());  // 35 -> 30
            menu_press(MenuKey::Left, qpc_now());  // 30 -> 25
            SetEvent(gate);
            bool shown = false;
            for (int i = 0; i < 200 && !shown; ++i) {
                Sleep(11);
                live_reload();
                shown = menu.model.values.periphery == 25 &&
                        GetPrivateProfileIntW(L"experiment", L"periphery_scale", 0, ini.c_str()) == 25;
            }
            std::printf("three held steps: the ini has %d, the menu shows %d\n",
                        GetPrivateProfileIntW(L"experiment", L"periphery_scale", 0, ini.c_str()), menu.model.values.periphery);
            ok &= shown;
            CloseHandle(gate);
        }
        menu.visible = false;
        experiment.views4 = false;
        live = Live{};
        DeleteFileW(ini.c_str());
        std::printf("settings menu: opened=%d, stepped into the ini=%d\n", opened ? 1 : 0, stepped ? 1 : 0);
        ok &= opened && stepped;
    }
    std::printf("RESULT: %s\n", ok ? "pass" : "FAIL");
    return ok ? 0 : 1;
}
