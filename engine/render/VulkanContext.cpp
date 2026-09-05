#include "VulkanContext.h"
#include <iostream>
#include <cstring>
#include <algorithm>
namespace afterlight {
static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                               VkDebugUtilsMessageTypeFlagsEXT,
                                               const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        static_cast<VulkanContext*>(user)->validationErrors++;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "[Vulkan] " << data->pMessage << "\n";
    return VK_FALSE;
}
void VulkanContext::initialize(HWND hwnd, bool validation) {
    const std::string bundledLayers = std::string(AFTERLIGHT_ROOT) + "/third_party/validation";
    if (GetEnvironmentVariableA("VK_LAYER_PATH", nullptr, 0) == 0 &&
        GetFileAttributesA((bundledLayers + "/VkLayer_khronos_validation.json").c_str()) !=
            INVALID_FILE_ATTRIBUTES)
        SetEnvironmentVariableA("VK_LAYER_PATH", bundledLayers.c_str());
    VK_CHECK(volkInitialize());
    uint32_t n = 0;
    VK_CHECK(vkEnumerateInstanceLayerProperties(&n, nullptr));
    std::vector<VkLayerProperties> layers(n);
    VK_CHECK(vkEnumerateInstanceLayerProperties(&n, layers.data()));
    validationActive = validation && std::any_of(layers.begin(), layers.end(), [](auto& l) {
                           return !strcmp(l.layerName, "VK_LAYER_KHRONOS_validation");
                       });
    std::vector<const char*> extensions{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    if (validationActive) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Afterlight";
    app.pEngineName = "Afterlight Engine";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    dbg.pfnUserCallback = callback;
    dbg.pUserData = this;
    VkValidationFeatureEnableEXT syncValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &syncValidation;
    validationFeatures.pNext = &dbg;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    if (validationActive) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
        ci.pNext = &validationFeatures;
    }
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
    volkLoadInstance(instance);
    if (validationActive)
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance, &dbg, nullptr, &debug));
    std::cout << "Validation layer: "
              << (validationActive ? "enabled" : "not installed (runtime checks still active)") << "\n";
    VkWin32SurfaceCreateInfoKHR surf{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surf.hinstance = GetModuleHandleW(nullptr);
    surf.hwnd = hwnd;
    VK_CHECK(vkCreateWin32SurfaceKHR(instance, &surf, nullptr, &surface));
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<VkPhysicalDevice> devices(n);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
    const std::vector<const char*> required{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    for (auto gpu : devices) {
        uint32_t ec = 0;
        VK_CHECK(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ec, nullptr));
        std::vector<VkExtensionProperties> available(ec);
        VK_CHECK(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ec, available.data()));
        bool supports = true;
        for (auto r : required)
            if (!std::any_of(available.begin(), available.end(),
                             [&](auto& e) { return !strcmp(e.extensionName, r); })) {
                supports = false;
                break;
            }
        if (!supports)
            continue;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR af{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = &af;
        af.pNext = &rq;
        vkGetPhysicalDeviceFeatures2(gpu, &features);
        if (!f12.bufferDeviceAddress || !f13.dynamicRendering || !f13.synchronization2 ||
            !af.accelerationStructure || !rq.rayQuery || !features.features.shaderStorageImageExtendedFormats)
            continue;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (props.apiVersion < VK_API_VERSION_1_3)
            continue;
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qc, queues.data());
        for (uint32_t q = 0; q < qc; q++) {
            VkBool32 present;
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, surface, &present));
            if (present && (queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                physical = gpu;
                family = q;
                break;
            }
        }
        if (physical)
            break;
    }
    if (!physical)
        throw std::runtime_error(
            "Requires Vulkan 1.3 GPU with accelerationStructure, rayQuery, bufferDeviceAddress, "
            "dynamicRendering, synchronization2. No raster-lighting fallback.");
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &asProperties;
    vkGetPhysicalDeviceProperties2(physical, &props2);
    std::cout << "GPU: " << properties.deviceName << " | Vulkan " << VK_VERSION_MAJOR(properties.apiVersion)
              << "." << VK_VERSION_MINOR(properties.apiVersion) << "\n";
    float priority = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f12.pNext = &f13;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR af{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    af.accelerationStructure = VK_TRUE;
    f13.pNext = &af;
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq.rayQuery = VK_TRUE;
    af.pNext = &rq;
    VkPhysicalDeviceFeatures base{};
    base.shaderStorageImageExtendedFormats = VK_TRUE;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.pNext = &f12;
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &q;
    dc.enabledExtensionCount = uint32_t(required.size());
    dc.ppEnabledExtensionNames = required.data();
    dc.pEnabledFeatures = &base;
    VK_CHECK(vkCreateDevice(physical, &dc, nullptr, &device));
    volkLoadDevice(device);
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.queueFamilyIndex = family;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(device, &cp, nullptr, &commandPool));
}
VulkanContext::~VulkanContext() {
    if (device) {
        vkDeviceWaitIdle(device);
        if (commandPool)
            vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (surface)
        vkDestroySurfaceKHR(instance, surface, nullptr);
    if (debug)
        vkDestroyDebugUtilsMessengerEXT(instance, debug, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
}
uint32_t VulkanContext::memoryType(uint32_t mask, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++)
        if ((mask & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("No suitable Vulkan memory type");
}
Buffer VulkanContext::buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = size;
    bc.usage = usage;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &bc, nullptr, &b.handle));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.handle, &req);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags =
        (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = req.size;
    ma.memoryTypeIndex = memoryType(req.memoryTypeBits, host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ma.pNext = &flags;
    VK_CHECK(vkAllocateMemory(device, &ma, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(device, b.handle, b.memory, 0));
    if (host)
        VK_CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    if (flags.flags) {
        VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addr.buffer = b.handle;
        b.address = vkGetBufferDeviceAddress(device, &addr);
    }
    return b;
}
void VulkanContext::destroy(Buffer& b) {
    if (b.mapped)
        vkUnmapMemory(device, b.memory);
    if (b.handle)
        vkDestroyBuffer(device, b.handle, nullptr);
    if (b.memory)
        vkFreeMemory(device, b.memory, nullptr);
    b = {};
}
Image VulkanContext::image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
    Image i;
    i.width = w;
    i.height = h;
    i.format = format;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateImage(device, &ci, nullptr, &i.handle));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, i.handle, &req);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = req.size;
    ma.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device, &ma, nullptr, &i.memory));
    VK_CHECK(vkBindImageMemory(device, i.handle, i.memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i.handle;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VkImageAspectFlags(format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                             : VK_IMAGE_ASPECT_COLOR_BIT),
                           0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device, &vi, nullptr, &i.view));
    return i;
}
void VulkanContext::destroy(Image& i) {
    if (i.view)
        vkDestroyImageView(device, i.view, nullptr);
    if (i.handle)
        vkDestroyImage(device, i.handle, nullptr);
    if (i.memory)
        vkFreeMemory(device, i.memory, nullptr);
    i = {};
}
VkCommandBuffer VulkanContext::beginOneTime() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer c;
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &c));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(c, &bi));
    return c;
}
void VulkanContext::endOneTime(VkCommandBuffer c) {
    VK_CHECK(vkEndCommandBuffer(c));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, commandPool, 1, &c);
}
void VulkanContext::transition(VkCommandBuffer c, Image& i, VkImageLayout next, VkPipelineStageFlags2 stage,
                               VkAccessFlags2 access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = i.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE
                                                           : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = i.layout == VK_IMAGE_LAYOUT_UNDEFINED
                          ? 0
                          : VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.dstStageMask = stage;
    b.dstAccessMask = access;
    b.oldLayout = i.layout;
    b.newLayout = next;
    b.image = i.handle;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VkImageAspectFlags(i.format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                              : VK_IMAGE_ASPECT_COLOR_BIT),
                          0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c, &d);
    i.layout = next;
}
void VulkanContext::barrier(VkCommandBuffer c) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c, &d);
}
} // namespace afterlight
