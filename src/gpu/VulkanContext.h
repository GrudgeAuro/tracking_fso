#pragma once

#include <vulkan/vulkan.h>
#include <vector>

struct GLFWwindow;

// VulkanContext owns everything that's the same regardless of what you're
// rendering: instance, device, queue, swapchain, command pool, per-frame
// sync objects.
//
// Dynamic rendering and synchronization2 are accessed through their KHR
// extension entry points rather than relying on Vulkan 1.3 core entry points.
// This is important for compatibility with the V3DV driver on Raspberry Pi 5.
class VulkanContext
{
public:
    static constexpr int kMaxFramesInFlight = 2;

    bool init(GLFWwindow* window, bool enableValidation);
    void shutdown();

    // Call when the window is resized / swapchain becomes suboptimal.
    bool recreateSwapchain();

    // Begin a frame: waits on the in-flight fence, acquires the next
    // swapchain image. Returns false if the swapchain needs recreating.
    bool beginFrame(uint32_t& imageIndex, VkCommandBuffer& cmd);

    // Submit the recorded command buffer and present.
    bool endFrame(uint32_t imageIndex, VkCommandBuffer cmd);

    // Slot whose fence was waited by the most recent beginFrame().
    uint32_t currentFrameSlot() const { return currentFrame_; }


    VkDevice device() const { return device_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamily() const { return queueFamily_; }
    VkCommandPool commandPool() const { return commandPool_; }

    VkExtent2D swapchainExtent() const
    {
        return swapchainExtent_;
    }

    VkFormat swapchainFormat() const
    {
        return swapchainFormat_;
    }

    VkImageView swapchainImageView(uint32_t i) const
    {
        return swapchainImageViews_[i];
    }

    VkImage swapchainImage(uint32_t i) const
    {
        return swapchainImages_[i];
    }

    // ------------------------------------------------------------
    // KHR synchronization2 / dynamic rendering entry points.
    //
    // These are loaded with vkGetDeviceProcAddr() after the logical
    // device has been created.
    // ------------------------------------------------------------

    PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2() const
    {
        return cmdPipelineBarrier2_;
    }

    PFN_vkCmdBeginRendering cmdBeginRendering() const
    {
        return cmdBeginRendering_;
    }

    PFN_vkCmdEndRendering cmdEndRendering() const
    {
        return cmdEndRendering_;
    }

private:
    bool createInstance(bool enableValidation);
    bool createSurface();
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createSwapchain();
    void destroySwapchain();
    bool createCommandPoolAndBuffers();
    bool createSyncObjects();

    // ------------------------------------------------------------
    // GLFW / Vulkan instance
    // ------------------------------------------------------------

    GLFWwindow* window_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    // ------------------------------------------------------------
    // Physical / logical device
    // ------------------------------------------------------------

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;

    // ------------------------------------------------------------
    // Swapchain
    // ------------------------------------------------------------

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};

    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;

    // ------------------------------------------------------------
    // Command buffers
    // ------------------------------------------------------------

    VkCommandPool commandPool_ = VK_NULL_HANDLE;

    // One command buffer per frame-in-flight.
    std::vector<VkCommandBuffer> commandBuffers_;

    // ------------------------------------------------------------
    // Per-frame synchronization
    // ------------------------------------------------------------

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;

    uint32_t currentFrame_ = 0;

    // ------------------------------------------------------------
    // Vulkan extension function pointers
    // ------------------------------------------------------------

    // VK_KHR_synchronization2
    PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2_ = nullptr;

    // VK_KHR_dynamic_rendering
    PFN_vkCmdBeginRendering cmdBeginRendering_ = nullptr;
    PFN_vkCmdEndRendering cmdEndRendering_ = nullptr;

    bool validationEnabled_ = false;
};
