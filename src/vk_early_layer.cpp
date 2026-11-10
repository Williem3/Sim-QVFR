// The early load's way in: sim-qvfr.dll as a pass-through Vulkan layer (VK_LAYER_SIM_qvfr_early). For a game with
// GameInfo::earlyLoad (X-Plane, which creates its shader modules before it starts OpenXR) the launcher names this
// layer in VK_INSTANCE_LAYERS and its manifest's folder in VK_ADD_LAYER_PATH. The Vulkan loader then loads the DLL at
// the game's first vkCreateInstance, before any device or shader module exists, and DllMain does the early start
// (early_load.cpp). The layer itself only passes every call on. This replaced loading the DLL into the suspended game
// with a remote thread, which antivirus scanners take for code injection.
//
// Only the few loader-interface types the layer needs are declared here (vk_layer.h, interface version 2), so the
// build needs no Vulkan SDK.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

using VkResult = int32_t;
constexpr VkResult VK_SUCCESS = 0;
constexpr VkResult VK_ERROR_INITIALIZATION_FAILED = -3;
using PFN_vkVoidFunction = void(__stdcall*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(__stdcall*)(void* instance, const char* name);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(__stdcall*)(void* device, const char* name);
using PFN_GetPhysicalDeviceProcAddr = PFN_vkVoidFunction(__stdcall*)(void* instance, const char* name);
using PFN_vkCreateInstance = VkResult(__stdcall*)(const void* createInfo, const void* allocator, void** instance);
using PFN_vkCreateDevice = VkResult(__stdcall*)(void* physicalDevice, const void* createInfo, const void* allocator,
                                                void** device);

constexpr int32_t kLoaderInstanceCreateInfo = 47;  // VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
constexpr int32_t kLoaderDeviceCreateInfo = 48;    // VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
constexpr int32_t kLayerLinkInfo = 0;              // VK_LAYER_LINK_INFO
constexpr int32_t kNegotiateInterfaceStruct = 1;   // LAYER_NEGOTIATE_INTERFACE_STRUCT

struct VkNegotiateLayerInterface {
    int32_t sType;
    void* pNext;
    uint32_t loaderLayerInterfaceVersion;
    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;
    PFN_GetPhysicalDeviceProcAddr pfnGetPhysicalDeviceProcAddr;
};
struct BaseIn {  // every Vk*CreateInfo starts with these two
    int32_t sType;
    const void* pNext;
};
struct VkLayerInstanceLink {
    VkLayerInstanceLink* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_GetPhysicalDeviceProcAddr pfnNextGetPhysicalDeviceProcAddr;
};
struct VkLayerInstanceCreateInfo {
    int32_t sType;
    const void* pNext;
    int32_t function;
    VkLayerInstanceLink* pLayerInfo;  // a union in vk_layer.h; the link member is the one used here
};
struct VkLayerDeviceLink {
    VkLayerDeviceLink* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
};
struct VkLayerDeviceCreateInfo {
    int32_t sType;
    const void* pNext;
    int32_t function;
    VkLayerDeviceLink* pLayerInfo;
};

// Dispatchable handles (instance, physical device, device, queue, command buffer) start with the loader's dispatch
// table pointer: handles of one instance or device share it.
void* key(void* handle) { return handle ? *static_cast<void**>(handle) : nullptr; }

std::mutex mutex;
std::unordered_map<void*, PFN_vkGetInstanceProcAddr> nextInstance;  // by instance (and its physical devices) key
std::unordered_map<void*, PFN_vkGetDeviceProcAddr> nextDevice;      // by device key

template <typename Info>
Info* link_info(const void* createInfo, int32_t sType) {
    for (auto* in = static_cast<const BaseIn*>(createInfo); in; in = static_cast<const BaseIn*>(in->pNext))
        if (in->sType == sType && reinterpret_cast<const Info*>(in)->function == kLayerLinkInfo)
            return const_cast<Info*>(reinterpret_cast<const Info*>(in));
    return nullptr;
}

PFN_vkVoidFunction __stdcall get_instance_proc(void* instance, const char* name);
PFN_vkVoidFunction __stdcall get_device_proc(void* device, const char* name);

VkResult __stdcall create_instance(const void* createInfo, const void* allocator, void** instance) {
    auto* chain = link_info<VkLayerInstanceCreateInfo>(createInfo, kLoaderInstanceCreateInfo);
    if (!chain || !chain->pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr next = chain->pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->pLayerInfo = chain->pLayerInfo->pNext;  // the next layer's link, for it to take
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(next(nullptr, "vkCreateInstance"));
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = create(createInfo, allocator, instance);
    if (result == VK_SUCCESS) {
        std::lock_guard lock(mutex);
        nextInstance[key(*instance)] = next;
    }
    return result;
}

VkResult __stdcall create_device(void* physicalDevice, const void* createInfo, const void* allocator, void** device) {
    auto* chain = link_info<VkLayerDeviceCreateInfo>(createInfo, kLoaderDeviceCreateInfo);
    if (!chain || !chain->pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr nextInstanceProc = chain->pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr nextDeviceProc = chain->pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->pLayerInfo = chain->pLayerInfo->pNext;
    // vkCreateDevice is an instance-level call; the next layer's comes through its vkGetInstanceProcAddr. A physical
    // device shares its instance's dispatch key, but the loader asks with a null instance here (vk_layer.h).
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(nextInstanceProc(nullptr, "vkCreateDevice"));
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = create(physicalDevice, createInfo, allocator, device);
    if (result == VK_SUCCESS) {
        std::lock_guard lock(mutex);
        nextDevice[key(*device)] = nextDeviceProc;
    }
    return result;
}

PFN_vkVoidFunction __stdcall get_instance_proc(void* instance, const char* name) {
    if (!name)
        return nullptr;
    if (!std::strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&get_instance_proc);
    if (!std::strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&get_device_proc);
    if (!std::strcmp(name, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(&create_instance);
    if (!std::strcmp(name, "vkCreateDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(&create_device);
    PFN_vkGetInstanceProcAddr next = nullptr;
    {
        std::lock_guard lock(mutex);
        const auto it = nextInstance.find(key(instance));
        if (it != nextInstance.end())
            next = it->second;
    }
    return next ? next(instance, name) : nullptr;
}

PFN_vkVoidFunction __stdcall get_device_proc(void* device, const char* name) {
    if (!name)
        return nullptr;
    if (!std::strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&get_device_proc);
    PFN_vkGetDeviceProcAddr next = nullptr;
    {
        std::lock_guard lock(mutex);
        const auto it = nextDevice.find(key(device));
        if (it != nextDevice.end())
            next = it->second;
    }
    return next ? next(device, name) : nullptr;
}

}  // namespace

// The one export the layer manifest (VkLayer_SIM_qvfr_early.json) names. Interface version 2: the loader gets
// vkGetInstanceProcAddr and vkGetDeviceProcAddr from here; no physical-device entry point.
extern "C" __declspec(dllexport) VkResult __stdcall vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (!v || v->sType != kNegotiateInterfaceStruct || v->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = &get_instance_proc;
    v->pfnGetDeviceProcAddr = &get_device_proc;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
