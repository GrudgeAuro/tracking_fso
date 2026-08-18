#pragma once

#include "IPipelineStage.h"
#include "VulkanContext.h"
#include "VulkanRawProcessor.h"
#include <array>
#include <memory>
#include <string>

struct GLFWwindow;

class VulkanDisplayStage : public IPipelineStage
{
public:
    explicit VulkanDisplayStage(const char* windowTitle = "PRBS Vision - GPU RAW",
                                bool enableValidation = true);
    ~VulkanDisplayStage() override;

    bool init(int width, int height) override;
    bool process(Frame& frame) override;
    void shutdown() override;
    const char* name() const override { return "VulkanDisplayStage (GPU RAW pipeline)"; }

    bool shouldContinue() const;

private:
    bool createDisplayResources();
    bool createDescriptors();
    bool createPipeline();
    void destroyDisplayResources();
    void releaseCompletedCameraFrame(uint32_t slot);
    void releaseAllPendingCameraFrames();

    const char* windowTitle_;
    bool enableValidation_;

    GLFWwindow* window_ = nullptr;
    VulkanContext ctx_;
    VulkanRawProcessor processor_;

    int width_ = 0;
    int height_ = 0;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    std::array<std::shared_ptr<std::function<void()>>, VulkanContext::kMaxFramesInFlight>
        pendingReleases_{};

    bool initialized_ = false;
};
