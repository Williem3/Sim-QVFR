// Offline check of the two_loops frame pairing against a fake runtime that enforces OpenXR's frame
// rules (wait before begin, begin/end strictly alternating), for ACE's normal call order and for a
// frame generator above the layer (two ends per begin), which must never arm.
#include "../src/xr_layer.cpp"
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

}  // namespace

int main() {
    simqvfr::game = &simqvfr::games::adapter_for(&ace::kGame);  // views4 below is ACE's
    const int errors = run(130, false);
    std::printf("ACE order: runtime waits=%d begins=%d ends=%d violations=%d endErrors=%d active=%d\n", rt.waits,
                rt.begins, rt.ends, rt.violations, errors, simqvfr::loops.active ? 1 : 0);
    // 30 unpaired frames, then 100 app frames paired into 50 display frames
    bool ok = rt.waits == 80 && rt.begins == 80 && rt.ends == 80 && rt.violations == 0 && errors == 0;

    run(130, true);
    std::printf("frame generator above: active=%d broken=%d (expected 0 and 1)\n", simqvfr::loops.active ? 1 : 0,
                simqvfr::loops.broken ? 1 : 0);
    ok &= !simqvfr::loops.active && simqvfr::loops.broken;

    // Live settings: an edited ini applies at the next xrEndFrame, but never between frames A and B.
    {
        using namespace simqvfr;
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
