#include <gsdl/graphics/win32_presenter.hpp>
#include "frame_bytes.hpp"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace gsdl::graphics {
namespace {
[[noreturn]] void fail(ErrorCode code, const char* message, VkResult result = VK_SUCCESS) {
    throw PresentationError(code, message, result);
}
void check(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return;
    const auto code = result == VK_ERROR_DEVICE_LOST ? ErrorCode::device_lost :
        result == VK_ERROR_SURFACE_LOST_KHR ? ErrorCode::surface_lost :
        (result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_EXTENSION_NOT_PRESENT ||
         result == VK_ERROR_FEATURE_NOT_PRESENT || result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        ? ErrorCode::unsupported_environment : ErrorCode::vulkan_failure;
    fail(code, operation, result);
}
template<class T, class U> T function_cast(U value) {
    static_assert(sizeof(T) == sizeof(U));
    T result{}; std::memcpy(&result, &value, sizeof(result)); return result;
}
template<class T, class F> std::vector<T> enumerate(F call, const char* operation) {
    for (int attempt = 0; attempt != 4; ++attempt) {
        std::uint32_t count{}; check(call(&count, static_cast<T*>(nullptr)), operation);
        if (count > 65'536) fail(ErrorCode::unsupported_environment, "Unreasonable Vulkan enumeration count");
        std::vector<T> values(count);
        if (!count) return values;
        const auto result = call(&count, values.data());
        if (result == VK_INCOMPLETE) continue;
        check(result, operation); values.resize(count); return values;
    }
    fail(ErrorCode::vulkan_failure, "Vulkan enumeration kept changing", VK_INCOMPLETE);
}
bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const auto& p) { return std::strcmp(p.extensionName, name) == 0; });
}
using detail::checked_frame_bytes;
const char* format_name(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    default: return "none";
    }
}
}

struct Win32Presenter::Impl {
    HWND window{};
    DWORD owner{};
    HMODULE loader{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkSurfaceKHR surface{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t family{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkSwapchainKHR swapchain{};
    VkExtent2D extent{};
    VkExtent2D last_requested{};
    VkFormat format{};
    std::vector<VkImage> images;
    VkCommandPool command_pool{};
    VkCommandBuffer command{};
    VkFence acquire_fence{}, submit_fence{}, present_fence{};
    VkSemaphore ready{};
    bool submit_pending{}, present_pending{}, acquire_pending{}, dirty{true}, poisoned{};
    std::uint32_t acquired_index{};
    bool image_acquired{};
    Diagnostics info;
    struct Buffer {
        VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{};
        std::size_t capacity{}; bool coherent{};
    } upload, readback;

#define VK_MEMBER(name) PFN_##name name{}
    VK_MEMBER(vkGetInstanceProcAddr); VK_MEMBER(vkGetDeviceProcAddr);
    VK_MEMBER(vkCreateInstance); VK_MEMBER(vkDestroyInstance);
    VK_MEMBER(vkEnumerateInstanceExtensionProperties); VK_MEMBER(vkEnumeratePhysicalDevices);
    VK_MEMBER(vkEnumerateDeviceExtensionProperties); VK_MEMBER(vkGetPhysicalDeviceProperties);
    VK_MEMBER(vkGetPhysicalDeviceFeatures2); VK_MEMBER(vkGetPhysicalDeviceMemoryProperties);
    VK_MEMBER(vkGetPhysicalDeviceQueueFamilyProperties); VK_MEMBER(vkGetPhysicalDeviceSurfaceSupportKHR);
    VK_MEMBER(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); VK_MEMBER(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VK_MEMBER(vkCreateWin32SurfaceKHR); VK_MEMBER(vkDestroySurfaceKHR); VK_MEMBER(vkCreateDevice);
    VK_MEMBER(vkDestroyDevice); VK_MEMBER(vkGetDeviceQueue); VK_MEMBER(vkDeviceWaitIdle);
    VK_MEMBER(vkCreateSwapchainKHR); VK_MEMBER(vkDestroySwapchainKHR); VK_MEMBER(vkGetSwapchainImagesKHR);
    VK_MEMBER(vkAcquireNextImageKHR); VK_MEMBER(vkQueuePresentKHR); VK_MEMBER(vkReleaseSwapchainImagesEXT);
    VK_MEMBER(vkCreateCommandPool); VK_MEMBER(vkDestroyCommandPool); VK_MEMBER(vkAllocateCommandBuffers);
    VK_MEMBER(vkResetCommandBuffer); VK_MEMBER(vkBeginCommandBuffer); VK_MEMBER(vkEndCommandBuffer);
    VK_MEMBER(vkCmdPipelineBarrier); VK_MEMBER(vkCmdCopyBufferToImage); VK_MEMBER(vkCmdCopyImageToBuffer);
    VK_MEMBER(vkCreateBuffer); VK_MEMBER(vkDestroyBuffer); VK_MEMBER(vkGetBufferMemoryRequirements);
    VK_MEMBER(vkAllocateMemory); VK_MEMBER(vkFreeMemory); VK_MEMBER(vkBindBufferMemory);
    VK_MEMBER(vkMapMemory); VK_MEMBER(vkUnmapMemory); VK_MEMBER(vkFlushMappedMemoryRanges);
    VK_MEMBER(vkInvalidateMappedMemoryRanges); VK_MEMBER(vkCreateFence); VK_MEMBER(vkDestroyFence);
    VK_MEMBER(vkWaitForFences); VK_MEMBER(vkResetFences); VK_MEMBER(vkCreateSemaphore);
    VK_MEMBER(vkDestroySemaphore); VK_MEMBER(vkQueueSubmit);
#undef VK_MEMBER

    template<class T> void instance_function(T& pointer, const char* name) {
        pointer = function_cast<T>(vkGetInstanceProcAddr(instance, name));
        if (!pointer) fail(ErrorCode::unsupported_environment, name);
    }
    template<class T> void device_function(T& pointer, const char* name) {
        pointer = function_cast<T>(vkGetDeviceProcAddr(device, name));
        if (!pointer) fail(ErrorCode::unsupported_environment, name);
    }
    void owner_check() const {
        if (GetCurrentThreadId() != owner) fail(ErrorCode::wrong_thread, "Presenter belongs to its construction/render thread");
    }
    void initialize(HWND hwnd) {
        owner = GetCurrentThreadId(); window = hwnd;
        DWORD process{};
        (void)GetWindowThreadProcessId(window, &process);
        if (!IsWindow(window) || process != GetCurrentProcessId()) fail(ErrorCode::invalid_input, "Presenter requires a live in-process HWND");
        // UI owns HWND; caller keeps its message pump alive until render teardown.
        // Vulkan objects are exclusively owned by this construction thread.
        std::array<wchar_t, 32768> system_directory{};
        const auto system_length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
        if (!system_length || system_length >= system_directory.size()) fail(ErrorCode::unsupported_environment, "Cannot identify the OS System32 directory");
        const auto expected_loader = std::wstring(system_directory.data(), system_length) + L"\\vulkan-1.dll";
        // An absolute path avoids reusing an already loaded same-basename DLL
        // from an unrelated host directory. Verify what Windows actually loaded.
        loader = LoadLibraryExW(expected_loader.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) fail(ErrorCode::unsupported_environment, "System32 Vulkan loader is unavailable");
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(loader, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length == path.size()) fail(ErrorCode::vulkan_failure, "Cannot identify loaded Vulkan loader");
        if (CompareStringOrdinal(path.data(), static_cast<int>(length), expected_loader.data(), static_cast<int>(expected_loader.size()), TRUE) != CSTR_EQUAL)
            fail(ErrorCode::unsupported_environment, "Refusing Vulkan loader outside the exact OS System32 path");
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (!bytes) fail(ErrorCode::vulkan_failure, "Invalid loader path");
        info.loader_path.resize(bytes);
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(length), info.loader_path.data(), bytes, nullptr, nullptr))
            fail(ErrorCode::vulkan_failure, "Cannot encode loader path");
        vkGetInstanceProcAddr = function_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!vkGetInstanceProcAddr) fail(ErrorCode::unsupported_environment, "Vulkan loader has no entry point");
        const auto version = function_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
        std::uint32_t api{};
        if (!version) fail(ErrorCode::unsupported_environment, "Vulkan 1.1 loader required");
        check(version(&api), "vkEnumerateInstanceVersion");
        if (api < VK_API_VERSION_1_1) fail(ErrorCode::unsupported_environment, "Vulkan 1.1 loader required");
        instance_function(vkCreateInstance, "vkCreateInstance");
        instance_function(vkEnumerateInstanceExtensionProperties, "vkEnumerateInstanceExtensionProperties");
        const auto extensions = enumerate<VkExtensionProperties>([&](auto* n, auto* p) { return vkEnumerateInstanceExtensionProperties(nullptr, n, p); }, "instance extensions");
        const char* required[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
        for (const auto* name : required) if (!has_extension(extensions, name)) fail(ErrorCode::unsupported_environment, name);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "3DGSDownloader native UI presenter"; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app; create.enabledExtensionCount = static_cast<std::uint32_t>(std::size(required)); create.ppEnabledExtensionNames = required;
        check(vkCreateInstance(&create, nullptr, &instance), "vkCreateInstance");
#define INSTANCE(name) instance_function(name, #name)
        INSTANCE(vkDestroyInstance); INSTANCE(vkGetDeviceProcAddr); INSTANCE(vkEnumeratePhysicalDevices);
        INSTANCE(vkEnumerateDeviceExtensionProperties); INSTANCE(vkGetPhysicalDeviceProperties);
        INSTANCE(vkGetPhysicalDeviceFeatures2); INSTANCE(vkGetPhysicalDeviceMemoryProperties);
        INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties); INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR);
        INSTANCE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); INSTANCE(vkGetPhysicalDeviceSurfaceFormatsKHR);
        INSTANCE(vkCreateWin32SurfaceKHR); INSTANCE(vkDestroySurfaceKHR); INSTANCE(vkCreateDevice);
#undef INSTANCE
        VkWin32SurfaceCreateInfoKHR surface_create{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface_create.hwnd = window; surface_create.hinstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
        check(vkCreateWin32SurfaceKHR(instance, &surface_create, nullptr, &surface), "vkCreateWin32SurfaceKHR");
        choose_device();
        const float priority = 1;
        VkDeviceQueueCreateInfo queue_create{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create.queueFamilyIndex = family; queue_create.queueCount = 1; queue_create.pQueuePriorities = &priority;
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        maintenance.swapchainMaintenance1 = VK_TRUE;
        const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
        VkDeviceCreateInfo device_create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_create.pNext = &maintenance; device_create.queueCreateInfoCount = 1; device_create.pQueueCreateInfos = &queue_create;
        device_create.enabledExtensionCount = 2; device_create.ppEnabledExtensionNames = device_extensions;
        check(vkCreateDevice(physical, &device_create, nullptr, &device), "vkCreateDevice");
#define DEVICE(name) device_function(name, #name)
        DEVICE(vkDestroyDevice); DEVICE(vkDeviceWaitIdle); DEVICE(vkGetDeviceQueue);
        DEVICE(vkCreateSwapchainKHR); DEVICE(vkDestroySwapchainKHR); DEVICE(vkGetSwapchainImagesKHR);
        DEVICE(vkAcquireNextImageKHR); DEVICE(vkQueuePresentKHR); DEVICE(vkReleaseSwapchainImagesEXT);
        DEVICE(vkCreateCommandPool); DEVICE(vkDestroyCommandPool); DEVICE(vkAllocateCommandBuffers);
        DEVICE(vkResetCommandBuffer); DEVICE(vkBeginCommandBuffer); DEVICE(vkEndCommandBuffer);
        DEVICE(vkCmdPipelineBarrier); DEVICE(vkCmdCopyBufferToImage); DEVICE(vkCmdCopyImageToBuffer);
        DEVICE(vkCreateBuffer); DEVICE(vkDestroyBuffer); DEVICE(vkGetBufferMemoryRequirements);
        DEVICE(vkAllocateMemory); DEVICE(vkFreeMemory); DEVICE(vkBindBufferMemory);
        DEVICE(vkMapMemory); DEVICE(vkUnmapMemory); DEVICE(vkFlushMappedMemoryRanges); DEVICE(vkInvalidateMappedMemoryRanges);
        DEVICE(vkCreateFence); DEVICE(vkDestroyFence); DEVICE(vkWaitForFences); DEVICE(vkResetFences);
        DEVICE(vkCreateSemaphore); DEVICE(vkDestroySemaphore); DEVICE(vkQueueSubmit);
#undef DEVICE
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pool_create{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_create.queueFamilyIndex = family; pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pool_create, nullptr, &command_pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = command_pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &allocate, &command), "vkAllocateCommandBuffers");
        const VkFenceCreateInfo fence_create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &fence_create, nullptr, &acquire_fence), "acquire fence");
        check(vkCreateFence(device, &fence_create, nullptr, &submit_fence), "submit fence");
        check(vkCreateFence(device, &fence_create, nullptr, &present_fence), "present fence");
        const VkSemaphoreCreateInfo semaphore_create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device, &semaphore_create, nullptr, &ready), "present semaphore");
        info.presentation_fences = true;
    }
    void choose_device() {
        const auto devices = enumerate<VkPhysicalDevice>([&](auto* n, auto* p) { return vkEnumeratePhysicalDevices(instance, n, p); }, "physical devices");
        int best_score = -1;
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_1) continue;
            const auto extensions = enumerate<VkExtensionProperties>([&](auto* n, auto* p) { return vkEnumerateDeviceExtensionProperties(candidate, nullptr, n, p); }, "device extensions");
            if (!has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has_extension(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) continue;
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &maintenance;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (!maintenance.swapchainMaintenance1) continue;
            std::uint32_t count{}; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
            if (count > 1024) continue;
            std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
            for (std::uint32_t i = 0; i < count; ++i) {
                VkBool32 supported{}; check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported), "surface support");
                if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !families[i].queueCount || !supported) continue;
                const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score <= best_score) continue;
                best_score = score; physical = candidate; family = i;
                info.device_name = properties.deviceName; info.api_version = properties.apiVersion;
                info.driver_version = properties.driverVersion; info.vendor_id = properties.vendorID; info.device_id = properties.deviceID;
            }
        }
        if (!physical) fail(ErrorCode::unsupported_environment, "No Vulkan 1.1 graphics/present device with swapchain maintenance1");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    }
    void destroy_buffer(Buffer& buffer) noexcept {
        if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
        if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
        if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
        buffer = {};
    }
    void ensure_buffer(Buffer& buffer, std::size_t bytes, VkBufferUsageFlags usage) {
        if (buffer.capacity >= bytes) return;
        destroy_buffer(buffer);
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = bytes; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &create, nullptr, &buffer.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
        if (requirements.size > max_frame_pixels * 4 + 65536) fail(ErrorCode::unsupported_environment, "Driver buffer allocation exceeds staging budget");
        std::uint32_t selected = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                selected = i;
                if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
            }
        }
        if (selected == UINT32_MAX) fail(ErrorCode::unsupported_environment, "No host-visible staging memory");
        buffer.coherent = (memory_properties.memoryTypes[selected].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = selected;
        check(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0), "vkBindBufferMemory");
        check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped), "vkMapMemory");
        buffer.capacity = bytes;
    }
    void mapped_sync(const Buffer& buffer, bool invalidate) {
        if (buffer.coherent) return;
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = buffer.memory; range.size = VK_WHOLE_SIZE;
        check(invalidate ? vkInvalidateMappedMemoryRanges(device, 1, &range) : vkFlushMappedMemoryRanges(device, 1, &range), "mapped memory synchronization");
    }
    void wait(VkFence fence, bool& pending) {
        if (!pending) return;
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        pending = false;
        check(vkResetFences(device, 1, &fence), "vkResetFences");
    }
    bool recreate(VkExtent2D requested) {
        wait(submit_fence, submit_pending); wait(present_fence, present_pending);
        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface capabilities");
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) fail(ErrorCode::unsupported_environment, "Swapchain lacks transfer-destination support");
        if (!(caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) || !(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
            fail(ErrorCode::unsupported_environment, "Opaque identity-oriented surface required");
        extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{
            std::clamp(requested.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(requested.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
        // The UI can minimize after present() checked HWND dimensions. A zero
        // surface extent is transient, not a malformed frame or poisoned device.
        // Keep the old swapchain until a nonzero replacement can be constructed.
        if (!extent.width || !extent.height) { dirty = true; return false; }
        (void)checked_frame_bytes(extent.width, extent.height);
        const auto formats = enumerate<VkSurfaceFormatKHR>([&](auto* n, auto* p) { return vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, n, p); }, "surface formats");
        format = VK_FORMAT_UNDEFINED;
        for (const auto desired : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            if (std::any_of(formats.begin(), formats.end(), [&](const auto& f) { return f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && (f.format == desired || f.format == VK_FORMAT_UNDEFINED); })) { format = desired; break; }
        }
        if (format == VK_FORMAT_UNDEFINED) fail(ErrorCode::unsupported_environment, "No RGBA8/BGRA8 sRGB-nonlinear surface format");
        info.readback_supported = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        auto count = caps.minImageCount + 1;
        if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
        if (count > 16) fail(ErrorCode::unsupported_environment, "Swapchain exceeds 16-image bound");
        if (swapchain) { vkDestroySwapchainKHR(device, swapchain, nullptr); swapchain = {}; images.clear(); }
        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface; create.minImageCount = count; create.imageFormat = format;
        create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; create.imageExtent = extent; create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (info.readback_supported ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; create.presentMode = VK_PRESENT_MODE_FIFO_KHR; create.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device, &create, nullptr, &swapchain), "vkCreateSwapchainKHR");
        images = enumerate<VkImage>([&](auto* n, auto* p) { return vkGetSwapchainImagesKHR(device, swapchain, n, p); }, "swapchain images");
        if (images.empty() || images.size() > 16) fail(ErrorCode::unsupported_environment, "Invalid swapchain image count");
        info.swapchain_format = format_name(format); ++info.swapchain_generation; dirty = false; last_requested = requested;
        return true;
    }
    void barrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                 VkAccessFlags source_access, VkAccessFlags destination_access,
                 VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = source_access; b.dstAccessMask = destination_access;
        b.oldLayout = old_layout; b.newLayout = new_layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    PresentResult present(const Rgba8Frame& frame, bool verify) {
        owner_check();
        if (poisoned) fail(ErrorCode::vulkan_failure, "Presenter is unusable after a Vulkan failure; destroy and recreate it");
        if (!IsWindow(window)) fail(ErrorCode::surface_lost, "Presenter HWND was destroyed");
        detail::validate_frame(frame);
        RECT rectangle{};
        if (!GetClientRect(window, &rectangle)) fail(ErrorCode::surface_lost, "Cannot query HWND client extent");
        if (IsIconic(window) || rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top) {
            dirty = true; return {PresentStatus::minimized, 0, 0, info.swapchain_generation, info.submitted_frames, false};
        }
        const VkExtent2D requested{static_cast<std::uint32_t>(rectangle.right - rectangle.left), static_cast<std::uint32_t>(rectangle.bottom - rectangle.top)};
        (void)checked_frame_bytes(requested.width, requested.height);
        try {
            if (dirty || !swapchain || requested.width != last_requested.width || requested.height != last_requested.height) {
                if (!recreate(requested)) return {PresentStatus::retry, 0, 0, info.swapchain_generation, info.submitted_frames, false};
            }
            if (verify && !info.readback_supported) fail(ErrorCode::unsupported_environment, "Surface lacks transfer-source readback support");
            const auto bytes = checked_frame_bytes(extent.width, extent.height);
            const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
            const auto expected = detail::resample(frame, extent.width, extent.height, bgra);
            ensure_buffer(upload, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            if (verify) ensure_buffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            std::memcpy(upload.mapped, expected.data(), bytes); mapped_sync(upload, false);
            const auto acquired = vkAcquireNextImageKHR(device, swapchain, 0, VK_NULL_HANDLE, acquire_fence, &acquired_index);
            if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT) return {PresentStatus::retry, extent.width, extent.height, info.swapchain_generation, info.submitted_frames, false};
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { dirty = true; return {PresentStatus::retry, extent.width, extent.height, info.swapchain_generation, info.submitted_frames, false}; }
            if (acquired != VK_SUBOPTIMAL_KHR) check(acquired, "vkAcquireNextImageKHR");
            image_acquired = true; acquire_pending = true; wait(acquire_fence, acquire_pending);
            if (acquired_index >= images.size()) fail(ErrorCode::vulkan_failure, "Driver returned invalid swapchain image index");
            check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
            const auto image = images[acquired_index];
            barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyBufferToImage(command, upload.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            if (verify) {
                barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
                VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
            }
            barrier(image, verify ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                verify ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready;
            check(vkQueueSubmit(queue, 1, &submit, submit_fence), "vkQueueSubmit"); submit_pending = true; ++info.submitted_frames;
            wait(submit_fence, submit_pending);
            bool matched = true;
            if (verify) { mapped_sync(readback, true); matched = std::memcmp(readback.mapped, expected.data(), bytes) == 0; }
            VkSwapchainPresentFenceInfoEXT fence_info{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
            fence_info.swapchainCount = 1; fence_info.pFences = &present_fence;
            VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present_info.pNext = &fence_info;
            present_info.waitSemaphoreCount = 1; present_info.pWaitSemaphores = &ready;
            present_info.swapchainCount = 1; present_info.pSwapchains = &swapchain; present_info.pImageIndices = &acquired_index;
            const auto result = vkQueuePresentKHR(queue, &present_info);
            // These results still enqueue the present operations and their fence.
            if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
                image_acquired = false; present_pending = true; wait(present_fence, present_pending);
            }
            if (result == VK_ERROR_OUT_OF_DATE_KHR) { dirty = true; return {PresentStatus::retry, extent.width, extent.height, info.swapchain_generation, info.submitted_frames, false}; }
            if (result != VK_SUBOPTIMAL_KHR) check(result, "vkQueuePresentKHR");
            ++info.accepted_presents;
            dirty = result == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR;
            if (!matched) fail(ErrorCode::readback_mismatch, "GPU readback does not match every expected swapchain byte");
            return {PresentStatus::presented, extent.width, extent.height, info.swapchain_generation, info.submitted_frames, verify};
        } catch (...) { poisoned = true; throw; }
    }
    ~Impl() noexcept {
        // Ownership is a public contract. No user callbacks and no worker thread
        // exist here. Partial initialization and device-lost paths remain RAII.
        if (device && vkWaitForFences) {
            for (const auto [fence, pending] : {std::pair{acquire_fence, acquire_pending}, std::pair{submit_fence, submit_pending}, std::pair{present_fence, present_pending}})
                if (pending) (void)vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        }
        if (device && vkDeviceWaitIdle) (void)vkDeviceWaitIdle(device);
        if (device && image_acquired && vkReleaseSwapchainImagesEXT) {
            VkReleaseSwapchainImagesInfoEXT release{VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT};
            release.swapchain = swapchain; release.imageIndexCount = 1; release.pImageIndices = &acquired_index;
            (void)vkReleaseSwapchainImagesEXT(device, &release);
        }
        if (device) {
            destroy_buffer(readback); destroy_buffer(upload);
            if (ready) vkDestroySemaphore(device, ready, nullptr);
            for (const auto fence : {acquire_fence, submit_fence, present_fence}) if (fence) vkDestroyFence(device, fence, nullptr);
            if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
            if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
            if (vkDestroyDevice) vkDestroyDevice(device, nullptr);
        }
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance && vkDestroyInstance) vkDestroyInstance(instance, nullptr);
        if (loader) FreeLibrary(loader);
    }
};

Win32Presenter::Win32Presenter(HWND window) : impl_(std::make_unique<Impl>()) { impl_->initialize(window); }
Win32Presenter::~Win32Presenter() noexcept = default;
PresentResult Win32Presenter::present(const Rgba8Frame& frame, bool verify) { return impl_->present(frame, verify); }
void Win32Presenter::resize() { impl_->owner_check(); impl_->dirty = true; }
Diagnostics Win32Presenter::diagnostics() const { impl_->owner_check(); return impl_->info; }
} // namespace gsdl::graphics
