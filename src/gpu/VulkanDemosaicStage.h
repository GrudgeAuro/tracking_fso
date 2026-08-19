#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

struct Frame;

class VulkanDemosaicStage
{
public:
    VulkanDemosaicStage(VkDevice device, VkPhysicalDevice phys, VkCommandPool cmdPool, VkQueue queue);
    ~VulkanDemosaicStage();

    // Process staging buffer containing raw 16-bit BGGR12 data via GPU compute shader.
    // - stagingBuffer: host-visible buffer with raw 16-bit samples (left-aligned 12-bit in 16-bit container)
    // - width/height: image dimensions
    // Returns true on success and makes the resulting image view available via outputImageView().
    bool processStagingBuffer(const std::vector<uint8_t>& stagingBuffer, uint32_t width, uint32_t height);

    VkImageView outputImageView() const { return outView_; }

private:
    VkDevice device_;
    VkPhysicalDevice phys_;
    VkCommandPool cmdPool_;
    VkQueue queue_;

    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    void* stagingMapped_ = nullptr;
    VkDeviceSize stagingCapacity_ = 0;

    VkImage inImage_ = VK_NULL_HANDLE;
    VkDeviceMemory inMemory_ = VK_NULL_HANDLE;

    VkImage outImage_ = VK_NULL_HANDLE;
    VkDeviceMemory outMemory_ = VK_NULL_HANDLE;
    VkImageView outView_ = VK_NULL_HANDLE;

    VkPipeline computePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSet_ = VK_NULL_HANDLE;

    bool createPipeline();
    void destroyPipeline();

    bool ensureStagingBuffer(VkDeviceSize size);
    bool uploadToInputImage(const std::vector<uint8_t>& data, uint32_t width, uint32_t height);
    bool createOutput(uint32_t width, uint32_t height);
};
