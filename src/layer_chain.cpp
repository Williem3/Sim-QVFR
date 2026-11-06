#include "layer_chain.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "launcher/json.h"

namespace simqvfr::chain {

namespace fs = std::filesystem;

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

}  // namespace

std::vector<std::string> split_names(const std::string& value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(';', start);
        if (end == std::string::npos)
            end = value.size();
        if (end > start)
            out.push_back(value.substr(start, end - start));  // (end - start): the loader's bug was `end`
        start = end + 1;
    }
    return out;
}

bool load(const std::wstring& folder, const std::string& name, Layer& out, std::string& why) {
    const fs::path manifestPath = fs::path(folder) / widen(name + ".json");
    std::ifstream in(manifestPath, std::ios::binary);
    if (!in) {
        why = "no manifest " + manifestPath.string();
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    json::Value manifest;
    const json::Value* layer = nullptr;
    if (!json::parse(text, manifest) || !(layer = manifest.find("api_layer")) || layer->string_at("name") != name) {
        why = "manifest " + manifestPath.string() + " is not " + name + "'s";
        return false;
    }
    fs::path library = widen(layer->string_at("library_path"));
    if (library.empty()) {
        why = "manifest without library_path";
        return false;
    }
    if (library.is_relative())
        library = manifestPath.parent_path() / library;
    std::string entry = "xrNegotiateLoaderApiLayerInterface";
    if (const json::Value* functions = layer->find("functions"); functions && !functions->string_at(entry).empty())
        entry = functions->string_at(entry);

    // Never freed: the game's instance calls into it until the process ends, as with the loader's layers.
    const HMODULE module = LoadLibraryW(library.c_str());
    if (!module) {
        why = "LoadLibrary " + library.string() + " failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    const auto negotiate =
        reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(GetProcAddress(module, entry.c_str()));
    if (!negotiate) {
        why = library.string() + " has no " + entry;
        return false;
    }
    // As the loader fills them (api_layer_interface.cpp).
    XrNegotiateLoaderInfo loaderInfo{};
    loaderInfo.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loaderInfo.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loaderInfo.structSize = sizeof(loaderInfo);
    loaderInfo.minInterfaceVersion = 1;
    loaderInfo.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateApiLayerRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);
    const XrResult r = negotiate(&loaderInfo, name.c_str(), &request);
    if (XR_FAILED(r) || !request.getInstanceProcAddr || !request.createApiLayerInstance) {
        why = "negotiation refused (" + std::to_string(r) + ")";
        return false;
    }
    out = {name, request.getInstanceProcAddr, request.createApiLayerInstance};
    return true;
}

const XrApiLayerNextInfo* link(const std::vector<Layer>& layers, const XrApiLayerNextInfo* self,
                               std::vector<XrApiLayerNextInfo>& storage) {
    if (layers.empty() || !self)
        return self;
    // Node i belongs to the layer it is handed to and carries the functions of the layer below it:
    // [self -> layers[0]], [layers[0] -> layers[1]], ..., [layers[n-1] -> what was below self].
    storage.assign(layers.size() + 1, XrApiLayerNextInfo{});
    for (size_t i = 0; i <= layers.size(); ++i) {
        XrApiLayerNextInfo& node = storage[i];
        node.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
        node.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
        node.structSize = sizeof(node);
        const char* owner = i == 0 ? self->layerName : layers[i - 1].name.c_str();
        strncpy_s(node.layerName, owner, _TRUNCATE);
        if (i < layers.size()) {
            node.nextGetInstanceProcAddr = layers[i].getInstanceProcAddr;
            node.nextCreateApiLayerInstance = layers[i].createApiLayerInstance;
            node.next = &storage[i + 1];
        } else {
            node.nextGetInstanceProcAddr = self->nextGetInstanceProcAddr;
            node.nextCreateApiLayerInstance = self->nextCreateApiLayerInstance;
            node.next = self->next;
        }
    }
    return &storage[0];
}

}  // namespace simqvfr::chain
