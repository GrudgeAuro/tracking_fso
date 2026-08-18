#pragma once

#include "Frame.h"
#include "VulkanContext.h"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>

// GPU-only RAW processing pipeline:
//
//   camera SBGGR16 DMA-BUF
//       -> Bayer monochrome (R16_UINT)
//       -> 3x3 median (R16_UINT)
//       -> local min/max contrast (R16_UINT)
//
// No camera pixels are copied or converted on the CPU.
class VulkanRawProcessor
{
public:
    bool init(VulkanContext& ctx, uint32_t width, uint32_t height);
    void shutdown();

    bool record(VkCommandBuffer cmd, const Frame& frame);

    VkImage outputImage() const { return contrastImage_; }
    VkImageView outputView() const { return contrastView_; }

private:
    struct ImportedBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    bool createImages();
    bool createMinMaxBuffer();
    bool createDescriptorLayouts();
    bool createDescriptorPool();
    bool createPipelines();

    bool getImportedBuffer(const Frame& frame, VkBuffer& outBuffer);
    bool importDmaBuf(const Frame& frame, ImportedBuffer& out);

    void destroyImportedBuffers();
    void destroyImages();
    void destroyPipelines();

    void transitionImage(VkCommandBuffer cmd,
                         VkImage image,
                         VkImageLayout oldLayout,
                         VkImageLayout newLayout,
                         VkPipelineStageFlags2 srcStage,
                         VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage,
                         VkAccessFlags2 dstAccess);

    VulkanContext* ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t tileSize_ = 128;
    uint32_t tileCountX_ = 0;
    uint32_t tileCountY_ = 0;

    std::unordered_map<int, ImportedBuffer> importedBuffers_;

    VkImage monoImage_ = VK_NULL_HANDLE;
    VkDeviceMemory monoMemory_ = VK_NULL_HANDLE;
    VkImageView monoView_ = VK_NULL_HANDLE;

    VkImage medianImage_ = VK_NULL_HANDLE;
    VkDeviceMemory medianMemory_ = VK_NULL_HANDLE;
    VkImageView medianView_ = VK_NULL_HANDLE;

    VkImage contrastImage_ = VK_NULL_HANDLE;
    VkDeviceMemory contrastMemory_ = VK_NULL_HANDLE;
    VkImageView contrastView_ = VK_NULL_HANDLE;

    VkBuffer minBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory minMemory_ = VK_NULL_HANDLE;
    VkBuffer maxBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory maxMemory_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout rawLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout imageToImageLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout minMaxLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout contrastLayout_ = VK_NULL_HANDLE;

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet rawSet_ = VK_NULL_HANDLE;
    VkDescriptorSet medianSet_ = VK_NULL_HANDLE;
    VkDescriptorSet minMaxSet_ = VK_NULL_HANDLE;
    VkDescriptorSet contrastSet_ = VK_NULL_HANDLE;

    VkPipelineLayout rawPipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout medianPipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout minMaxPipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout contrastPipelineLayout_ = VK_NULL_HANDLE;

    VkPipeline rawPipeline_ = VK_NULL_HANDLE;
    VkPipeline medianPipeline_ = VK_NULL_HANDLE;
    VkPipeline minMaxPipeline_ = VK_NULL_HANDLE;
    VkPipeline contrastPipeline_ = VK_NULL_HANDLE;

    bool monoShaderRead_ = false;
    bool medianShaderRead_ = false;
    bool contrastShaderRead_ = false;
};
