#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

struct Frame;

class VulkanDemosaicStage
{
public:
    VulkanDemosaicStage(VkDevice device, VkPhysicalDevice phys, VkCommandPool cmdPool, VkQueue queue);
    ~VulkanDemosaicStage();

    // Import the camera plane dmabuf (fd) and run the compute demosaic.
    // - dmabufFd: file descriptor for the dmabuf containing left-aligned 12-bit
    //   samples packed in a 16-bit container (value = sample12 << 4).
    // - width/height: image dimensions
    // Returns true on success and makes the resulting image view available via outputImageView().
    bool processDmabuf(int dmabufFd, uint32_t width, uint32_t height);

    VkImageView outputImageView() const { return outView_; }

private:
    VkDevice device_;
    VkPhysicalDevice phys_;
    VkCommandPool cmdPool_;
    VkQueue queue_;

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

    bool importInput(int fd, uint32_t width, uint32_t height);
    bool createOutput(uint32_t width, uint32_t height);
};
