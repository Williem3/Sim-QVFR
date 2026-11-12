// Offline check of src/layer_chain.h: the names parsed as the loader should have (OpenComposite's loader's
// bug left only the first and last), fake layers walking the linked chain as real layers do (each checks
// its node's name and calls the next layer's create with the next node), and a real LoadLibrary and
// negotiation of the built sim-qvfr.dll from a manifest.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "layer_chain.h"

using namespace simqvfr;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

std::vector<std::string> visited;

// A layer's xrCreateApiLayerInstance, as mbucchia's and Cheeky's do it: its own node or refuse, then down.
template <int N>
XrResult XRAPI_CALL fakeCreate(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    static const char* const kNames[] = {"XR_APILAYER_A", "XR_APILAYER_B", "XR_APILAYER_C"};
    if (!layerInfo || !layerInfo->nextInfo || std::strcmp(layerInfo->nextInfo->layerName, kNames[N]) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;
    visited.push_back(kNames[N]);
    XrApiLayerCreateInfo next = *layerInfo;
    next.nextInfo = layerInfo->nextInfo->next;
    return layerInfo->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
}

template <int N>
XrResult XRAPI_CALL fakeGipa(XrInstance, const char*, PFN_xrVoidFunction*) {
    return XR_SUCCESS;
}

// What the loader puts at the bottom: the runtime's xrCreateInstance behind the terminator.
XrResult XRAPI_CALL runtimeCreate(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    visited.push_back(layerInfo->nextInfo ? "runtime (with a node left over)" : "runtime");
    *instance = reinterpret_cast<XrInstance>(0x1234);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL runtimeGipa(XrInstance, const char*, PFN_xrVoidFunction*) { return XR_SUCCESS; }

}  // namespace

int SIM_QVFR_TEST_MAIN(int, char** argv) {
    // The launcher's list, and what OpenComposite's loader made of it (its substr(start, end) bug).
    const std::string list = "XR_APILAYER_SIM_qvfr;XR_APILAYER_MBUCCHIA_quad_views_foveated;XR_APILAYER_MBUCCHIA_toolkit;"
                             "XR_APILAYER_VIRTUALDESKTOP_oculus_compatibility;XR_APILAYER_CHEEKY_foveated_dlss";
    const std::vector<std::string> names = chain::split_names(list);
    check(names.size() == 5 && names[1] == "XR_APILAYER_MBUCCHIA_quad_views_foveated" &&
              names[2] == "XR_APILAYER_MBUCCHIA_toolkit" && names[4] == "XR_APILAYER_CHEEKY_foveated_dlss",
          "chain: every name of a five-layer list comes out whole");
    {
        // The bug, reproduced: the second name came out as the user's log showed it.
        const size_t start = list.find(';') + 1, end = list.find(';', start);
        check(list.substr(start, end) == "XR_APILAYER_MBUCCHIA_quad_views_foveated;XR_APILAYER_MBUCCHIA",
              "chain: substr(start, end) reproduces OpenComposite's loader's mangled name");
    }
    check(chain::split_names(";A;;B;").size() == 2 && chain::split_names("").empty(), "chain: empty entries are skipped");

    // The loader's node for this layer: its functions are the runtime's (nothing else below).
    XrApiLayerNextInfo self{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, XR_API_LAYER_NEXT_INFO_STRUCT_VERSION,
                            sizeof(XrApiLayerNextInfo)};
    strncpy_s(self.layerName, "XR_APILAYER_SIM_qvfr", _TRUNCATE);
    self.nextGetInstanceProcAddr = runtimeGipa;
    self.nextCreateApiLayerInstance = runtimeCreate;
    self.next = nullptr;
    const std::vector<chain::Layer> layers = {{"XR_APILAYER_A", fakeGipa<0>, fakeCreate<0>},
                                              {"XR_APILAYER_B", fakeGipa<1>, fakeCreate<1>},
                                              {"XR_APILAYER_C", fakeGipa<2>, fakeCreate<2>}};
    std::vector<XrApiLayerNextInfo> storage;
    const XrApiLayerNextInfo* ours = chain::link(layers, &self, storage);
    check(ours && std::strcmp(ours->layerName, "XR_APILAYER_SIM_qvfr") == 0 && ours->nextGetInstanceProcAddr == fakeGipa<0>,
          "chain: this layer's node now leads to the first chained layer");
    XrApiLayerCreateInfo createInfo{};
    createInfo.nextInfo = ours->next;
    XrInstance instance = XR_NULL_HANDLE;
    const XrResult r = ours->nextCreateApiLayerInstance(nullptr, &createInfo, &instance);
    const std::vector<std::string> want = {"XR_APILAYER_A", "XR_APILAYER_B", "XR_APILAYER_C", "runtime"};
    check(XR_SUCCEEDED(r) && visited == want && instance == reinterpret_cast<XrInstance>(0x1234),
          "chain: xrCreateInstance passes A, B, C in order, each on its own node, then the runtime");
    std::vector<XrApiLayerNextInfo> none;
    check(chain::link({}, &self, none) == &self, "chain: no layers to chain leaves the loader's node as it is");

    // A real layer DLL, loaded and negotiated from a manifest: the built sim-qvfr.dll next to this test.
    const fs::path here = fs::path(argv[0]).parent_path();
    const fs::path folder = fs::temp_directory_path() / "simqvfr-chain-test";
    fs::create_directories(folder);
    {
        std::ofstream m(folder / "XR_APILAYER_SIM_qvfr.json", std::ios::trunc);
        m << "{\"file_format_version\":\"1.0.0\",\"api_layer\":{\"name\":\"XR_APILAYER_SIM_qvfr\",\"library_path\":\""
          << (here / "sim-qvfr.dll").generic_string()
          << "\",\"api_version\":\"1.0\",\"implementation_version\":\"1\"}}";
    }
    chain::Layer loaded;
    std::string why;
    const bool ok = chain::load(folder.wstring(), "XR_APILAYER_SIM_qvfr", loaded, why);
    if (!ok)
        std::printf("     (%s)\n", why.c_str());
    check(ok && loaded.getInstanceProcAddr && loaded.createApiLayerInstance,
          "chain: a real layer DLL loads and negotiates from its manifest");
    check(!chain::load(folder.wstring(), "XR_APILAYER_MISSING", loaded, why) && why.find("no manifest") == 0,
          "chain: a layer without a manifest is refused, with the reason");
    {
        std::ofstream m(folder / "XR_APILAYER_GONE.json", std::ios::trunc);
        m << "{\"file_format_version\":\"1.0.0\",\"api_layer\":{\"name\":\"XR_APILAYER_GONE\",\"library_path\":\"gone.dll\","
             "\"api_version\":\"1.0\",\"implementation_version\":\"1\"}}";
    }
    check(!chain::load(folder.wstring(), "XR_APILAYER_GONE", loaded, why) && why.find("LoadLibrary") == 0,
          "chain: a missing library is refused, with the reason");
    std::error_code ec;
    fs::remove_all(folder, ec);

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
