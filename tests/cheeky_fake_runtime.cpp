// A stand-in for Cheeky's CheekyFoveatedDLSSRuntime.dll (tests/cheeky_host_test.cpp): the five runtime
// ABI exports, checking what the host passes as Cheeky's runtime does (size, abi, configuration
// folder, attachment), and recording the calls for the test to read back through Fake_State.
#include <cstdint>
#include <cstring>
#include <string>

namespace {

struct Start {
    uint32_t size, abi;
    const wchar_t* configDirectory;
    uint32_t renderer;
    void *device, *queue;
    uint64_t* attachment;
    uint32_t host;
};

struct State {
    uint32_t starts, ticks, nullTicks, commands, lastRenderer, host;
    void *lastDevice, *lastQueue;
    char lastCommand[256];
} state;

uint64_t views[2] = {};  // evaluation_calls of the snapshot's two views, grown by each tick

}  // namespace

extern "C" __declspec(dllexport) bool CheekyRuntime_Start(const Start* input) {
    if (!input || input->size != 56 || input->abi != 1 || !input->configDirectory || !*input->configDirectory ||
        !input->attachment || input->host > 2)
        return false;
    ++state.starts;
    state.host = input->host;
    *input->attachment = 7;
    return true;
}

extern "C" __declspec(dllexport) void CheekyRuntime_Tick(uint64_t attachment, uint32_t renderer, void* device,
                                                         void* queue) {
    if (attachment != 7)
        return;
    ++state.ticks;
    state.nullTicks += device == nullptr;
    state.lastRenderer = renderer;
    state.lastDevice = device;
    state.lastQueue = queue;
    if (device) {
        views[0] += 1;  // one evaluation per frame on the first view, two on the second
        views[1] += 2;
    }
}

extern "C" __declspec(dllexport) bool CheekyRuntime_Command(uint64_t attachment, const char* command) {
    if (attachment != 7 || !command)
        return false;
    ++state.commands;
    strncpy_s(state.lastCommand, command, _TRUNCATE);
    return true;
}

extern "C" __declspec(dllexport) bool CheekyRuntime_Snapshot(char* output, uint32_t capacity) {
    const std::string json =
        "{\"settings\":{},\"view_details\":[{\"id\":\"0xa\",\"eye\":\"Left\",\"evaluations\":1,\"evaluation_calls\":" +
        std::to_string(views[0]) +
        ",\"api\":\"DX12\",\"mapping\":\"Calibrated\",\"region\":\"Unknown\",\"input_width\":2265,\"input_height\":1896,"
        "\"output_width\":4530,\"output_height\":3792},{\"id\":\"0xb\",\"eye\":\"Right\",\"evaluation_calls\":" +
        std::to_string(views[1]) +
        ",\"api\":\"DX12\",\"mapping\":\"Default quad order\",\"region\":\"Center\",\"input_width\":800,"
        "\"input_height\":700,\"output_width\":1600,\"output_height\":1400}],\"view_details_total\":2}";
    if (!output || json.size() >= capacity)
        return false;
    std::memcpy(output, json.c_str(), json.size() + 1);
    return true;
}

extern "C" __declspec(dllexport) void CheekyRuntime_Detach(uint64_t) {}

extern "C" __declspec(dllexport) const State* Fake_State() { return &state; }
