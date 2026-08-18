#include "VulkanContext.h"

#include <GLFW/glfw3.h>
#include <iostream>
#include <set>
#include <algorithm>
#include <limits>
#include <cstring>

namespace
{
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "[Vulkan] " << data->pMessage << "\n";
    return VK_FALSE;
}
} // namespace

bool VulkanContext::init(GLFWwindow* window, bool enableValidation)
{
    window_ = window;
    validationEnabled_ = enableValidation;

    if (!createInstance(enableValidation)) return false;
    if (!createSurface()) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    if (!createSwapchain()) return false;
    if (!createCommandPoolAndBuffers()) return false;
    if (!createSyncObjects()) return false;

    return true;
}

bool VulkanContext::createInstance(bool enableValidation)
{
    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "prbs_vision";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "prbs_vision";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);
    if (enableValidation)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    std::vector<const char*> layers;
    if (enableValidation)
        layers.push_back("VK_LAYER_KHRONOS_validation");

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanContext] vkCreateInstance failed\n";
        return false;
    }

    if (enableValidation)
    {
        auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (createFn)
        {
            VkDebugUtilsMessengerCreateInfoEXT dci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debugCallback;
            createFn(instance_, &dci, nullptr, &debugMessenger_);
        }
    }

    return true;
}

bool VulkanContext::createSurface()
{
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanContext] glfwCreateWindowSurface failed\n";
        return false;
    }
    return true;
}


bool VulkanContext::pickPhysicalDevice()
{
    uint32_t count = 0;

    vkEnumeratePhysicalDevices(
        instance_,
        &count,
        nullptr
    );

    if (count == 0)
    {
        std::cerr
            << "[VulkanContext] No Vulkan-capable devices found\n";
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);

    vkEnumeratePhysicalDevices(
        instance_,
        &count,
        devices.data()
    );

    for (VkPhysicalDevice dev : devices)
    {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extensionCount, extensions.data());

        auto hasExtension = [&](const char* name)
        {
            return std::any_of(extensions.begin(), extensions.end(),
                [&](const VkExtensionProperties& e)
                { return std::strcmp(e.extensionName, name) == 0; });
        };

        if (!hasExtension(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) ||
            !hasExtension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) ||
            !hasExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
        {
            continue;
        }

        uint32_t qCount = 0;

        vkGetPhysicalDeviceQueueFamilyProperties(
            dev,
            &qCount,
            nullptr
        );

        std::vector<VkQueueFamilyProperties> qProps(qCount);

        vkGetPhysicalDeviceQueueFamilyProperties(
            dev,
            &qCount,
            qProps.data()
        );

        for (uint32_t i = 0; i < qCount; ++i)
        {
            VkBool32 presentSupport = VK_FALSE;

            vkGetPhysicalDeviceSurfaceSupportKHR(
                dev,
                i,
                surface_,
                &presentSupport
            );

            if ((qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                presentSupport)
            {
                physicalDevice_ = dev;
                queueFamily_ = i;
                return true;
            }
        }
    }

    std::cerr
        << "[VulkanContext] No device with a combined "
           "graphics+present queue found\n";

    return false;
}


bool VulkanContext::createLogicalDevice()
{
    float priority = 1.0f;

    VkDeviceQueueCreateInfo qci{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO
    };
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME
    };

    // Query KHR dynamic rendering + synchronization2 support.
    VkPhysicalDeviceSynchronization2FeaturesKHR supportedSync2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR
    };

    VkPhysicalDeviceDynamicRenderingFeaturesKHR supportedDynamicRendering{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR
    };

    supportedDynamicRendering.pNext = &supportedSync2;

    VkPhysicalDeviceFeatures2 supportedFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2
    };

    supportedFeatures.pNext = &supportedDynamicRendering;

    vkGetPhysicalDeviceFeatures2(
        physicalDevice_,
        &supportedFeatures
    );

    if (!supportedDynamicRendering.dynamicRendering)
    {
        std::cerr
            << "[VulkanContext] Dynamic rendering is not supported"
            << std::endl;
        return false;
    }

    if (!supportedSync2.synchronization2)
    {
        std::cerr
            << "[VulkanContext] Synchronization2 is not supported"
            << std::endl;
        return false;
    }

    // Enable KHR dynamic rendering + synchronization2.
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2Features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR
    };

    sync2Features.synchronization2 = VK_TRUE;

    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRenderingFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR
    };

    dynamicRenderingFeatures.dynamicRendering = VK_TRUE;
    dynamicRenderingFeatures.pNext = &sync2Features;

    VkPhysicalDeviceFeatures2 features2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2
    };

    features2.pNext = &dynamicRenderingFeatures;

    // Create logical device.
    VkDeviceCreateInfo ci{
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
    };

    ci.pNext = &features2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount =
        static_cast<uint32_t>(deviceExtensions.size());
    ci.ppEnabledExtensionNames = deviceExtensions.data();

    VkResult result = vkCreateDevice(
        physicalDevice_,
        &ci,
        nullptr,
        &device_
    );

    if (result != VK_SUCCESS)
    {
        std::cerr
            << "[VulkanContext] vkCreateDevice failed: "
            << result
            << std::endl;
        return false;
    }

    // Get queue.
    vkGetDeviceQueue(
        device_,
        queueFamily_,
        0,
        &queue_
    );

    // Load KHR command functions explicitly.
    cmdPipelineBarrier2_ =
        reinterpret_cast<PFN_vkCmdPipelineBarrier2KHR>(
            vkGetDeviceProcAddr(
                device_,
                "vkCmdPipelineBarrier2KHR"
            )
        );

    cmdBeginRendering_ =
        reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
            vkGetDeviceProcAddr(
                device_,
                "vkCmdBeginRenderingKHR"
            )
        );

    cmdEndRendering_ =
        reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
            vkGetDeviceProcAddr(
                device_,
                "vkCmdEndRenderingKHR"
            )
        );

    if (!cmdPipelineBarrier2_ ||
        !cmdBeginRendering_ ||
        !cmdEndRendering_)
    {
        std::cerr
            << "[VulkanContext] Failed to load KHR Vulkan functions"
            << std::endl;

        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;

        return false;
    }

    return true;
}

bool VulkanContext::createSwapchain()
{
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto& f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosenFormat = f;
            break;
        }
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, presentModes.data());
    VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync, always available

    VkExtent2D extent;
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
    {
        extent = caps.currentExtent;
    }
    else
    {
        int w, h;
        glfwGetFramebufferSize(window_, &w, &h);
        extent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosenFormat.format;
    ci.imageColorSpace = chosenFormat.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = chosenPresentMode;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanContext] vkCreateSwapchainKHR failed\n";
        return false;
    }

    swapchainFormat_ = chosenFormat.format;
    swapchainExtent_ = extent;

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actualCount, nullptr);
    swapchainImages_.resize(actualCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &actualCount, swapchainImages_.data());

    swapchainImageViews_.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i)
    {
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = swapchainImages_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapchainFormat_;
        vci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device_, &vci, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS)
        {
            std::cerr << "[VulkanContext] vkCreateImageView failed for swapchain image " << i << "\n";
            return false;
        }
    }

    return true;
}

void VulkanContext::destroySwapchain()
{
    for (VkImageView view : swapchainImageViews_)
        vkDestroyImageView(device_, view, nullptr);
    swapchainImageViews_.clear();
    swapchainImages_.clear();

    if (swapchain_ != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanContext::recreateSwapchain()
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    while (w == 0 || h == 0)
    {
        glfwGetFramebufferSize(window_, &w, &h);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device_);
    destroySwapchain();
    return createSwapchain();
}

bool VulkanContext::createCommandPoolAndBuffers()
{
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &commandPool_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanContext] vkCreateCommandPool failed\n";
        return false;
    }

    commandBuffers_.resize(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kMaxFramesInFlight;
    if (vkAllocateCommandBuffers(device_, &ai, commandBuffers_.data()) != VK_SUCCESS)
    {
        std::cerr << "[VulkanContext] vkAllocateCommandBuffers failed\n";
        return false;
    }
    return true;
}

bool VulkanContext::createSyncObjects()
{
    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    renderFinishedSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);

    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < kMaxFramesInFlight; ++i)
    {
        if (vkCreateSemaphore(device_, &sci, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &sci, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fci, nullptr, &inFlightFences_[i]) != VK_SUCCESS)
        {
            std::cerr << "[VulkanContext] Failed to create sync objects\n";
            return false;
        }
    }
    return true;
}


bool VulkanContext::beginFrame(
    uint32_t& imageIndex,
    VkCommandBuffer& cmd)
{
    vkWaitForFences(
        device_,
        1,
        &inFlightFences_[currentFrame_],
        VK_TRUE,
        UINT64_MAX
    );

    VkResult result = vkAcquireNextImageKHR(
        device_,
        swapchain_,
        UINT64_MAX,
        imageAvailableSemaphores_[currentFrame_],
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        return false;

    if (result != VK_SUCCESS &&
        result != VK_SUBOPTIMAL_KHR)
    {
        std::cerr
            << "[VulkanContext] vkAcquireNextImageKHR failed: "
            << result << "\n";
        return false;
    }

    vkResetFences(
        device_,
        1,
        &inFlightFences_[currentFrame_]
    );

    cmd = commandBuffers_[currentFrame_];

    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };

    VkResult beginResult =
        vkBeginCommandBuffer(cmd, &bi);

    if (beginResult != VK_SUCCESS)
    {
        std::cerr
            << "[VulkanContext] vkBeginCommandBuffer failed: "
            << beginResult << "\n";
        return false;
    }

    return true;
}


bool VulkanContext::endFrame(
    uint32_t imageIndex,
    VkCommandBuffer cmd)
{
    VkResult result = vkEndCommandBuffer(cmd);

    if (result != VK_SUCCESS)
    {
        std::cerr
            << "[VulkanContext] vkEndCommandBuffer failed: "
            << result << "\n";
        return false;
    }

    VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{
        VK_STRUCTURE_TYPE_SUBMIT_INFO
    };

    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores =
        &imageAvailableSemaphores_[currentFrame_];

    submit.pWaitDstStageMask =
        &waitStage;

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores =
        &renderFinishedSemaphores_[currentFrame_];

    result = vkQueueSubmit(
        queue_,
        1,
        &submit,
        inFlightFences_[currentFrame_]
    );

    if (result != VK_SUCCESS)
    {
        std::cerr
            << "[VulkanContext] vkQueueSubmit failed: "
            << result << "\n";
        return false;
    }

    VkPresentInfoKHR present{
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR
    };

    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores =
        &renderFinishedSemaphores_[currentFrame_];

    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(
        queue_,
        &present
    );

    currentFrame_ =
        (currentFrame_ + 1) % kMaxFramesInFlight;

    if (result == VK_ERROR_OUT_OF_DATE_KHR ||
        result == VK_SUBOPTIMAL_KHR)
    {
        return false;
    }

    if (result != VK_SUCCESS)
    {
        std::cerr
            << "[VulkanContext] vkQueuePresentKHR failed: "
            << result << "\n";
        return false;
    }

    return true;
}


void VulkanContext::shutdown()
{
    if (device_ == VK_NULL_HANDLE)
        return;

    vkDeviceWaitIdle(device_);

    for (int i = 0; i < kMaxFramesInFlight; ++i)
    {
        vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
        vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
        vkDestroyFence(device_, inFlightFences_[i], nullptr);
    }
    if (commandPool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(device_, commandPool_, nullptr);

    destroySwapchain();

    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;

    if (surface_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance_, surface_, nullptr);

    if (debugMessenger_ != VK_NULL_HANDLE)
    {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn)
            destroyFn(instance_, debugMessenger_, nullptr);
    }

    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
}
