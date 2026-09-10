#include "renderCore/vulkan/VulkanContext.h"
#include <iostream>
#include <cstring>
#include <algorithm>
namespace whimsical {
static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                               VkDebugUtilsMessageTypeFlagsEXT,
                                               const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        static_cast<VulkanContext*>(user)->validationErrors++;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "[Vulkan] " << data->pMessage << "\n";
    return VK_FALSE;
}
void VulkanContext::initialize(HWND hwnd, bool validation, bool rayQueries) {
    const std::string bundledLayers = std::string(WHIMSICAL_ROOT) + "/third_party/validation";
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
    std::vector<const char*> extensions;
    if (hwnd)
        extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    uint32_t extensionCount = 0;
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
    std::vector<VkExtensionProperties> instanceExtensions(extensionCount);
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, instanceExtensions.data()));
    debugLabels = validationActive ||
                  std::any_of(instanceExtensions.begin(), instanceExtensions.end(), [](const auto& e) {
                      return !strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                  });
    if (debugLabels)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validationActive) {
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Whimsical";
    app.pEngineName = "Whimsical Engine";
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
              << (validationActive ? "enabled"
                  : validation     ? "not installed (runtime checks still active)"
                                   : "off by default in Release (pass --validation)")
              << "\n";
    if (hwnd) {
        VkWin32SurfaceCreateInfoKHR surf{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surf.hinstance = GetModuleHandleW(nullptr);
        surf.hwnd = hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance, &surf, nullptr, &surface));
    }
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<VkPhysicalDevice> devices(n);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
    std::vector<const char*> required;
    if (hwnd)
        required.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (rayQueries) {
        required.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        required.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        required.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }
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
        f13.pNext = rayQueries ? &af : nullptr;
        af.pNext = &rq;
        vkGetPhysicalDeviceFeatures2(gpu, &features);
        if (!f12.bufferDeviceAddress || !f12.shaderSampledImageArrayNonUniformIndexing ||
            !features.features.samplerAnisotropy || !f13.dynamicRendering || !f13.synchronization2 ||
            (rayQueries && (!af.accelerationStructure || !rq.rayQuery)) ||
            !features.features.shaderStorageImageExtendedFormats)
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
            VkBool32 present = VK_TRUE;
            if (surface)
                VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, surface, &present));
            if (present && (queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                physical = gpu;
                family = q;
                queueCount_ = std::min(2u, queues[q].queueCount);
                break;
            }
        }
        if (physical)
            break;
    }
    if (!physical)
        throw std::runtime_error("No Vulkan 1.3 graphics/compute device meets the requested capabilities");
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = rayQueries ? &asProperties : nullptr;
    vkGetPhysicalDeviceProperties2(physical, &props2);
    std::cout << "GPU: " << properties.deviceName << " | Vulkan " << VK_VERSION_MAJOR(properties.apiVersion)
              << "." << VK_VERSION_MINOR(properties.apiVersion) << "\n";
    float priorities[] = {1, 1};
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family;
    q.queueCount = queueCount_;
    q.pQueuePriorities = priorities;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f12.pNext = &f13;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR af{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    af.accelerationStructure = VK_TRUE;
    f13.pNext = rayQueries ? &af : nullptr;
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq.rayQuery = VK_TRUE;
    af.pNext = &rq;
    VkPhysicalDeviceFeatures base{};
    base.shaderStorageImageExtendedFormats = VK_TRUE;
    base.samplerAnisotropy = VK_TRUE;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.pNext = &f12;
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &q;
    dc.enabledExtensionCount = uint32_t(required.size());
    dc.ppEnabledExtensionNames = required.data();
    dc.pEnabledFeatures = &base;
    VK_CHECK(vkCreateDevice(physical, &dc, nullptr, &device));
    volkLoadDevice(device);
    for (uint32_t i = 0; i < queueCount_; ++i)
        vkGetDeviceQueue(device, family, i, &queues_[i]);
    std::cout << "GPU execution queues: " << queueCount_ << " (shared family)\n";
}
VulkanContext::~VulkanContext() {
    if (device) {
        vkDeviceWaitIdle(device);
        for (const auto& entry : commandPools_)
            vkDestroyCommandPool(device, entry.second, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (surface)
        vkDestroySurfaceKHR(instance, surface, nullptr);
    if (debug)
        vkDestroyDebugUtilsMessengerEXT(instance, debug, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
}
uint32_t VulkanContext::memoryType(uint32_t mask, VkMemoryPropertyFlags required,
                                   VkMemoryPropertyFlags preferred) const {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
        const auto flags = memoryProperties.memoryTypes[i].propertyFlags;
        if ((mask & (1u << i)) && (flags & required) == required) {
            if ((flags & preferred) == preferred)
                return i;
            if (fallback == UINT32_MAX)
                fallback = i;
        }
    }
    if (fallback != UINT32_MAX)
        return fallback;
    throw std::runtime_error("No suitable Vulkan memory type");
}
Buffer VulkanContext::buffer(VkDeviceSize size, VkBufferUsageFlags usage, BufferMemory memory) {
    const bool host = memory != BufferMemory::Device;
    Buffer b;
    b.generation = nextResourceGeneration();
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
    const auto required = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                               : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ma.memoryTypeIndex =
        memoryType(req.memoryTypeBits, required,
                   memory == BufferMemory::Readback ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
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
Image VulkanContext::image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                           uint32_t mipLevels) {
    Image i;
    i.generation = nextResourceGeneration();
    i.width = w;
    i.height = h;
    i.format = format;
    i.mipLevels = mipLevels;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = mipLevels;
    ci.arrayLayers = 1;
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
                           0, mipLevels, 0, 1};
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
VkCommandBuffer VulkanContext::allocateCommand() {
    // Recording a command buffer externally synchronizes its pool. Give each
    // execution context/native scope a pool, rather than locking whole recordings.
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = family;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer command;
    auto result = vkAllocateCommandBuffers(device, &info, &command);
    if (result != VK_SUCCESS) {
        vkDestroyCommandPool(device, pool, nullptr);
        VK_CHECK(result);
    }
    std::lock_guard<std::mutex> lock(commandMutex_);
    commandPools_.emplace(command, pool);
    return command;
}
void VulkanContext::freeCommand(VkCommandBuffer command) {
    VkCommandPool pool;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        pool = commandPools_.at(command);
        commandPools_.erase(command);
    }
    vkDestroyCommandPool(device, pool, nullptr);
}
void VulkanContext::submit(const VkSubmitInfo& info, VkFence fence, bool compute) {
    auto index = compute ? queueCount_ - 1 : 0;
    std::lock_guard<std::mutex> lock(queueMutex_[index]);
    VK_CHECK(vkQueueSubmit(queues_[index], 1, &info, fence));
}
VkResult VulkanContext::present(const VkPresentInfoKHR& info) {
    std::lock_guard<std::mutex> lock(queueMutex_[0]);
    return vkQueuePresentKHR(queues_[0], &info);
}
void VulkanContext::waitIdle() {
    std::scoped_lock lock(queueMutex_[0], queueMutex_[1]);
    VK_CHECK(vkDeviceWaitIdle(device));
}
void VulkanContext::execute(const std::function<void(VkCommandBuffer)>& record) {
    VkFence fence;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    VkCommandBuffer command = VK_NULL_HANDLE;
    try {
        command = allocateCommand();
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command, &begin));
        record(command);
        VK_CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        submit(info, fence);
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    } catch (...) {
        if (command)
            freeCommand(command);
        vkDestroyFence(device, fence, nullptr);
        throw;
    }
    freeCommand(command);
    vkDestroyFence(device, fence, nullptr);
}
void VulkanContext::uploadImage(Image& image, const void* pixels, size_t bytes,
                                VkPipelineStageFlags2 consumerStage, VkAccessFlags2 consumerAccess,
                                VkImageLayout finalLayout, VkFilter mipFilter) {
    auto staging = buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload);
    std::memcpy(staging.mapped, pixels, bytes);
    try {
        execute([&](VkCommandBuffer command) {
            transition(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {image.width, image.height, 1};
            vkCmdCopyBufferToImage(command, staging.handle, image.handle,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            auto barrier = [&](uint32_t level, VkImageLayout before, VkImageLayout after,
                               VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
                VkImageMemoryBarrier2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                info.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                info.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                info.dstStageMask = stage;
                info.dstAccessMask = access;
                info.oldLayout = before;
                info.newLayout = after;
                info.image = image.handle;
                info.srcQueueFamilyIndex = info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &info;
                vkCmdPipelineBarrier2(command, &dependency);
            };
            int32_t width = int32_t(image.width), height = int32_t(image.height);
            for (uint32_t level = 1; level < image.mipLevels; ++level) {
                barrier(level - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                VkImageBlit region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                region.srcOffsets[1] = {width, height, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                region.dstOffsets[1] = {std::max(1, width / 2), std::max(1, height / 2), 1};
                vkCmdBlitImage(command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, mipFilter);
                barrier(level - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, finalLayout, consumerStage,
                        consumerAccess);
                width = std::max(1, width / 2);
                height = std::max(1, height / 2);
            }
            barrier(image.mipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, finalLayout, consumerStage,
                    consumerAccess);
        });
        image.layout = finalLayout;
    } catch (...) {
        destroy(staging);
        throw;
    }
    destroy(staging);
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
                          0, i.mipLevels, 0, 1};
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
} // namespace whimsical
