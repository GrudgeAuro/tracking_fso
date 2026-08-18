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
        // Support both buffer-backed and image-backed imports so we can
        // experiment with either path during debugging.
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    bool createImages();
    bool createMinMaxBuffer();
    bool createDescriptorLayouts();
    bool createDescriptorPool();
    bool createPipelines();

    // Buffer-backed import (legacy / fallback).
    bool getImportedBuffer(const Frame& frame, VkBuffer& outBuffer);
    bool importDmaBuf(const Frame& frame, ImportedBuffer& out);

    // Image-backed import (preferred on platforms that support it).
    bool getImportedImage(const Frame& frame, VkImageView& outView);
    bool importDmaBufAsImage(const Frame& frame, ImportedBuffer& out);

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

    // Two raw descriptor/pipeline variants: buffer-backed and image-backed.
    VkDescriptorSetLayout rawBufferLayout_ = VK_NULL_HANDLE; // binding 0 = storage buffer, 1 = mono image
    VkDescriptorSetLayout rawImageLayout_ = VK_NULL_HANDLE;  // binding 0 = storage image, 1 = mono image

    VkDescriptorSetLayout imageToImageLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout minMaxLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout contrastLayout_ = VK_NULL_HANDLE;

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet rawBufferSet_ = VK_NULL_HANDLE;
    VkDescriptorSet rawImageSet_ = VK_NULL_HANDLE;
    VkDescriptorSet medianSet_ = VK_NULL_HANDLE;
    VkDescriptorSet minMaxSet_ = VK_NULL_HANDLE;
    VkDescriptorSet contrastSet_ = VK_NULL_HANDLE;

    VkPipelineLayout rawBufferPipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout rawImagePipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout medianPipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout minMaxPipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout contrastPipelineLayout_ = VK_NULL_HANDLE;

    VkPipeline rawBufferPipeline_ = VK_NULL_HANDLE;
    VkPipeline rawImagePipeline_ = VK_NULL_HANDLE;
    VkPipeline medianPipeline_ = VK_NULL_HANDLE;
    VkPipeline minMaxPipeline_ = VK_NULL_HANDLE;
    VkPipeline contrastPipeline_ = VK_NULL_HANDLE;

    bool monoShaderRead_ = false;
    bool medianShaderRead_ = false;
    bool contrastShaderRead_ = false;
};
