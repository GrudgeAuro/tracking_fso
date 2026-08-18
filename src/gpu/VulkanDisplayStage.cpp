#include "VulkanDisplayStage.h"
#include "VkUtils.h"

#include <GLFW/glfw3.h>
#include <iostream>

namespace
{
constexpr VkFormat kProcessFormat = VK_FORMAT_R16_UINT;
}

VulkanDisplayStage::VulkanDisplayStage(const char* windowTitle, bool enableValidation)
    : windowTitle_(windowTitle), enableValidation_(enableValidation)
{
}

VulkanDisplayStage::~VulkanDisplayStage()
{
    shutdown();
}

bool VulkanDisplayStage::init(int width, int height)
{
    width_ = width;
    height_ = height;

    if (!glfwInit())
    {
        std::cerr << "[VulkanDisplayStage] glfwInit() failed\n";
        return false;
    }

    if (!glfwVulkanSupported())
    {
        std::cerr << "[VulkanDisplayStage] Vulkan not supported\n";
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(width_, height_, windowTitle_, nullptr, nullptr);
    if (!window_)
    {
        std::cerr << "[VulkanDisplayStage] glfwCreateWindow() failed\n";
        return false;
    }

    if (!ctx_.init(window_, enableValidation_))
        return false;

    if (!processor_.init(ctx_, static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)))
        return false;

    if (!createDisplayResources()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    initialized_ = true;
    return true;
}

bool VulkanDisplayStage::createDisplayResources()
{
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(ctx_.device(), &sci, nullptr, &sampler_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDisplayStage] vkCreateSampler failed\n";
        return false;
    }

    return true;
}

bool VulkanDisplayStage::createDescriptors()
{
    VkDevice device = ctx_.device();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 1;
    lci.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device, &lci, nullptr, &descriptorSetLayout_) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(device, &pci, nullptr, &descriptorPool_) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = descriptorPool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &descriptorSetLayout_;

    if (vkAllocateDescriptorSets(device, &dai, &descriptorSet_) != VK_SUCCESS)
        return false;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = processor_.outputView();
    imageInfo.sampler = sampler_;

    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    return true;
}

bool VulkanDisplayStage::createPipeline()
{
    VkDevice device = ctx_.device();

    VkShaderModule vertModule =
        VkUtils::loadShaderModule(device, "shaders/fullscreen.vert.spv");
    VkShaderModule fragModule =
        VkUtils::loadShaderModule(device, "shaders/display_16bit.frag.spv");

    if (!vertModule || !fragModule)
    {
        std::cerr << "[VulkanDisplayStage] Failed to load display shaders\n";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
        return false;

    VkFormat swapFormat = ctx_.swapchainFormat();
    VkPipelineRenderingCreateInfo renderingInfo{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO
    };
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &swapFormat;

    VkGraphicsPipelineCreateInfo pci{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
    };
    pci.pNext = &renderingInfo;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vertexInput;
    pci.pInputAssemblyState = &inputAssembly;
    pci.pViewportState = &viewportState;
    pci.pRasterizationState = &rasterizer;
    pci.pMultisampleState = &multisample;
    pci.pColorBlendState = &blend;
    pci.pDynamicState = &dynamicState;
    pci.layout = pipelineLayout_;

    VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_);

    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);

    if (result != VK_SUCCESS)
    {
        std::cerr << "[VulkanDisplayStage] vkCreateGraphicsPipelines failed\n";
        return false;
    }

    return true;
}

void VulkanDisplayStage::releaseCompletedCameraFrame(uint32_t slot)
{
    auto& pending = pendingReleases_[slot];
    if (pending && *pending)
    {
        auto cb = std::move(*pending);
        pending.reset();
        cb();
    }
}

void VulkanDisplayStage::releaseAllPendingCameraFrames()
{
    for (auto& pending : pendingReleases_)
    {
        if (pending && *pending)
        {
            auto cb = std::move(*pending);
            pending.reset();
            cb();
        }
    }
}

bool VulkanDisplayStage::process(Frame& frame)
{
    if (!initialized_ || frame.empty())
        return false;

    glfwPollEvents();

    uint32_t imageIndex = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    if (!ctx_.beginFrame(imageIndex, cmd))
    {
        ctx_.recreateSwapchain();
        return shouldContinue();
    }

    const uint32_t slot = ctx_.currentFrameSlot();
    releaseCompletedCameraFrame(slot);

    if (!processor_.record(cmd, frame))
    {
        frame.release();
        return false;
    }

    auto barrier2 = ctx_.cmdPipelineBarrier2();
    auto beginRendering = ctx_.cmdBeginRendering();
    auto endRendering = ctx_.cmdEndRendering();

    if (!barrier2 || !beginRendering || !endRendering)
    {
        frame.release();
        return false;
    }

    VkImageMemoryBarrier2 toColor{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2
    };
    toColor.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.image = ctx_.swapchainImage(imageIndex);
    toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDependencyInfo toColorDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    toColorDep.imageMemoryBarrierCount = 1;
    toColorDep.pImageMemoryBarriers = &toColor;
    barrier2(cmd, &toColorDep);

    VkRenderingAttachmentInfo attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO
    };
    attachment.imageView = ctx_.swapchainImageView(imageIndex);
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};

    VkRenderingInfo renderingInfo{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    renderingInfo.renderArea = {{ 0, 0 }, ctx_.swapchainExtent()};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &attachment;

    beginRendering(cmd, &renderingInfo);

    VkExtent2D extent = ctx_.swapchainExtent();
    VkViewport viewport{
        0.0f, 0.0f,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        0.0f, 1.0f
    };
    VkRect2D scissor{{ 0, 0 }, extent};

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    endRendering(cmd);

    VkImageMemoryBarrier2 toPresent{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2
    };
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.image = ctx_.swapchainImage(imageIndex);
    toPresent.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDependencyInfo presentDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    presentDep.imageMemoryBarrierCount = 1;
    presentDep.pImageMemoryBarriers = &toPresent;
    barrier2(cmd, &presentDep);

    // The camera buffer is recycled only after this frame's fence signals.
    pendingReleases_[slot] = frame.releaseCallback;

    if (!ctx_.endFrame(imageIndex, cmd))
    {
        // If submission/presentation failed, make sure the camera buffer is
        // not left permanently associated with an unsignaled frame slot.
        vkDeviceWaitIdle(ctx_.device());
        releaseCompletedCameraFrame(slot);
        if (pendingReleases_[slot])
        {
            auto cb = std::move(*pendingReleases_[slot]);
            pendingReleases_[slot].reset();
            if (cb) cb();
        }
        ctx_.recreateSwapchain();
    }

    return shouldContinue();
}

bool VulkanDisplayStage::shouldContinue() const
{
    if (!window_)
        return false;
    return !glfwWindowShouldClose(window_) &&
           glfwGetKey(window_, GLFW_KEY_ESCAPE) != GLFW_PRESS;
}

void VulkanDisplayStage::destroyDisplayResources()
{
    VkDevice device = ctx_.device();
    if (!device)
        return;

    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
    if (sampler_) vkDestroySampler(device, sampler_, nullptr);

    sampler_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSetLayout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
}

void VulkanDisplayStage::shutdown()
{
    if (!ctx_.device() && !window_)
        return;

    if (ctx_.device())
        vkDeviceWaitIdle(ctx_.device());

    releaseAllPendingCameraFrames();
    destroyDisplayResources();
    processor_.shutdown();
    ctx_.shutdown();

    if (window_)
        glfwDestroyWindow(window_);
    window_ = nullptr;

    glfwTerminate();
    initialized_ = false;
}
