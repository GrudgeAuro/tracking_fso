#pragma once

#include "IPipelineStage.h"
#include "VulkanContext.h"
#include <string>

struct GLFWwindow;

// Fundamental stage: uploads each Frame's luma channel (16-bit R16_UINT) to a
// single-channel GPU texture and draws it full-screen via Vulkan. The GPU
// fragment shader maps the 16-bit mono values to 8-bit for display, but the
// underlying Frame passed to the pipeline remains CV_16UC1 so signal
// processing can operate on the full 16-bit luma values.
//
// This owns the VulkanContext (instance/device/swapchain) since it's the
// only stage today. When the PRBS correlator stage is added, promote
// VulkanContext ownership up into main.cpp and pass a reference in here and
// to the new stage, so both share one device/queue/command pool.
class VulkanDisplayStage : public IPipelineStage
{
public:
    explicit VulkanDisplayStage(const char* windowTitle = "PRBS Vision - Y Channel",
                                 bool enableValidation = true);
    ~VulkanDisplayStage() override;

    bool init(int width, int height) override;
    bool process(const Frame& frame) override;
    void shutdown() override;
    const char* name() const override { return "VulkanDisplayStage"; }

    bool shouldContinue() const;

private:
    bool createTextureResources();
    bool createDescriptors();
    bool createPipeline();
    void uploadFrame(VkCommandBuffer cmd, const Frame& frame);
    void destroyTextureResources();

    const char* windowTitle_;
    bool enableValidation_;

    GLFWwindow* window_ = nullptr;
    VulkanContext ctx_;

    int width_ = 0, height_ = 0;

    // Luma texture (R16_UINT) sampled by the fragment shader + its staging buffer.
    VkImage textureImage_ = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory_ = VK_NULL_HANDLE;
    VkImageView textureView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    void* stagingMapped_ = nullptr;
    bool textureLayoutIsShaderRead_ = false;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    bool initialized_ = false;
};
