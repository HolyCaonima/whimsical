#pragma once
#include <windows.h>
#include <volk.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <atomic>
namespace afterlight {
inline void vkCheck(VkResult r, const char* operation) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(r));
}
#define VK_CHECK(x) ::afterlight::vkCheck((x), #x)
// Allocation identity survives handle reuse. Owners that create raw Vulkan objects use
// this same source of identities when constructing their wrappers.
inline uint64_t nextResourceGeneration() {
    static std::atomic<uint64_t> next{0};
    return ++next;
}
struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
    uint64_t generation = 0;
};
struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    uint32_t mipLevels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t generation = 0;
};
enum class BufferMemory { Device, Upload, Readback };
class VulkanContext {
  public:
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::atomic<uint32_t> validationErrors{0};
    bool validationActive = false;
    bool debugLabels = false;
    void initialize(HWND, bool validation);
    ~VulkanContext();
    Buffer buffer(VkDeviceSize, VkBufferUsageFlags, BufferMemory memory = BufferMemory::Device);
    void destroy(Buffer&);
    Image image(uint32_t, uint32_t, VkFormat, VkImageUsageFlags, uint32_t mipLevels = 1);
    void destroy(Image&);
    VkCommandBuffer beginOneTime();
    void endOneTime(VkCommandBuffer);
    void transition(VkCommandBuffer, Image&, VkImageLayout,
                    VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VkAccessFlags2 dstAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    static void barrier(VkCommandBuffer);
    uint32_t memoryType(uint32_t, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const;
};
} // namespace afterlight
