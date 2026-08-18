#include "VulkanDisplayStage.h"
#include "VkUtils.h"
#include "VulkanDemosaicStage.h"

#include <GLFW/glfw3.h>
#include <iostream>
#include <cstring>

namespace
{
// Use a 16-bit unsigned integer format for display sampling so the fragment
// shader can sample the GPU-produced R16_UINT image directly as unsigned
// integers and convert to normalized floats for preview.
constexpr VkFormat kYChannelFormat = VK_FORMAT_R16_UINT;
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
        std::cerr << "[VulkanDisplayStage] Vulkan not supported by GLFW/loader on this system\n";
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // no GL context -- we're driving Vulkan
    window_ = glfwCreateWindow(width_, height_, windowTitle_, nullptr, nullptr);
    if (!window_)
    {
        std::cerr << "[VulkanDisplayStage] glfwCreateWindow() failed\n";
        return false;
    }

    if (!ctx_.init(window_, enableValidation_))
        return false;

    // Create the GPU demosaic stage now that the VulkanContext exists. If the
    // platform supports dmabuf import the stage will be used; otherwise it
    // will not be invoked.
    demosaicStage_ = new VulkanDemosaicStage(ctx_.device(), ctx_.physicalDevice(), ctx_.commandPool(), ctx_.queue());

    if (!createTextureResources()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    initialized_ = true;
    return true;
}

bool VulkanDisplayStage::createTextureResources()
{
    VkDevice device = ctx_.device();
    VkPhysicalDevice phys = ctx_.physicalDevice();

    if (!VkUtils::createImage2D(device, phys, width_, height_, kYChannelFormat,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 textureImage_, textureMemory_))
        return false;

    textureView_ = VkUtils::createImageView2D(device, textureImage_, kYChannelFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    if (textureView_ == VK_NULL_HANDLE) return false;

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    if (vkCreateSampler(device, &sci, nullptr, &sampler_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDisplayStage] vkCreateSampler failed\n";
        return false;
    }

    // Staging buffer holds 2 bytes per pixel for CV_16UC1 -> R16 image.
    const VkDeviceSize stagingSize = static_cast<VkDeviceSize>(width_) * static_cast<VkDeviceSize>(height_) * 2ull;
    if (!VkUtils::createBuffer(device, phys, stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                stagingBuffer_, stagingMemory_))
        return false;

    vkMapMemory(device, stagingMemory_, 0, stagingSize, 0, &stagingMapped_);
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
    {
        std::cerr << "[VulkanDisplayStage] vkCreateDescriptorSetLayout failed\n";
        return false;
    }

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &pci, nullptr, &descriptorPool_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDisplayStage] vkCreateDescriptorPool failed\n";
        return false;
    }

    VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = descriptorPool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(device, &dai, &descriptorSet_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDisplayStage] vkAllocateDescriptorSets failed\n";
        return false;
    }

    // Initially point descriptor at the CPU-backed texture image. When the
    // GPU demosaic path is used we update the descriptor to point at the
    // VulkanDemosaicStage's output image view.
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = textureView_;
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

    VkShaderModule vertModule = VkUtils::loadShaderModule(device, "shaders/fullscreen.vert.spv");
    VkShaderModule fragModule = VkUtils::loadShaderModule(device, "shaders/y_channel.frag.spv");
    if (!vertModule || !fragModule)
    {
        std::cerr << "[VulkanDisplayStage] Failed to load shaders. Run from the build directory "
                     "(shaders/*.spv is generated next to the executable) or fix the path.\n";
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

    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    // No bindings/attributes: the fullscreen triangle is generated in the
    // vertex shader from gl_VertexIndex alone.

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynStates;

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &descriptorSetLayout_;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDisplayStage] vkCreatePipelineLayout failed\n";
        return false;
    }

    // Dynamic rendering: pipeline declares the color attachment format it
    // will render to instead of referencing a VkRenderPass object.
    VkFormat swapFormat = ctx_.swapchainFormat();
    VkPipelineRenderingCreateInfo renderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &swapFormat;

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.pNext = &renderingInfo;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vertexInput;
    pci.pInputAssemblyState = &inputAssembly;
    pci.pViewportState = &viewportState;
    pci.pRasterizationState = &rasterizer;
    pci.pMultisampleState = &multisample;
    pci.pColorBlendState = &colorBlend;
    pci.pDynamicState = &dynamicState;
    pci.layout = pipelineLayout_;

    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_);

    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);

    if (result != VK_SUCCESS)
    {
        std::cerr << "[VulkanDisplayStage] vkCreateGraphicsPipelines failed\n";
        return false;
    }
    return true;
}


void VulkanDisplayStage::uploadFrame(
    VkCommandBuffer cmd,
    const Frame& frame)
{
    if (frame.image.empty())
    {
        std::cerr << "[uploadFrame] ERROR: frame.image is empty\n";
        return;
    }

    if (!stagingMapped_)
    {
        std::cerr
            << "[uploadFrame] ERROR: stagingMapped_ is null\n";
        return;
    }

    // Expect CV_16UC1 with 2 bytes per pixel
    const size_t rowBytes = static_cast<size_t>(width_) * 2;

    uint8_t* dst =
        static_cast<uint8_t*>(stagingMapped_);

    if (frame.image.type() != CV_16UC1) {
        std::cerr << "[uploadFrame] ERROR: expected CV_16UC1, got type=" << frame.image.type() << "\n";
        return;
    }

    if (frame.image.isContinuous() &&
        static_cast<size_t>(frame.image.step) == rowBytes)
    {
        std::memcpy(
            dst,
            frame.image.data,
            rowBytes * static_cast<size_t>(height_)
        );
    }
    else
    {
        for (int row = 0; row < height_; ++row)
        {
            std::memcpy(
                dst + row * rowBytes,
                frame.image.ptr(row),
                rowBytes
            );
        }
    }

    // ------------------------------------------------------------
    // Get KHR synchronization2 function.
    // ------------------------------------------------------------

    auto cmdPipelineBarrier2 =
        ctx_.cmdPipelineBarrier2();

    if (!cmdPipelineBarrier2)
    {
        std::cerr
            << "[uploadFrame] ERROR: "
               "ctx_.cmdPipelineBarrier2() is NULL\n";
        return;
    }

    // ------------------------------------------------------------
    // Transition texture -> TRANSFER_DST.
    // ------------------------------------------------------------

    VkImageMemoryBarrier2 toTransferDst{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2
    };

    toTransferDst.srcStageMask =
        textureLayoutIsShaderRead_
            ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
            : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;

    toTransferDst.srcAccessMask =
        textureLayoutIsShaderRead_
            ? VK_ACCESS_2_SHADER_READ_BIT
            : 0;

    toTransferDst.dstStageMask =
        VK_PIPELINE_STAGE_2_COPY_BIT;

    toTransferDst.dstAccessMask =
        VK_ACCESS_2_TRANSFER_WRITE_BIT;

    toTransferDst.oldLayout =
        textureLayoutIsShaderRead_
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;

    toTransferDst.newLayout =
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    toTransferDst.image =
        textureImage_;

    toTransferDst.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        1,
        0,
        1
    };

    VkDependencyInfo dep1{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO
    };

    dep1.imageMemoryBarrierCount = 1;
    dep1.pImageMemoryBarriers = &toTransferDst;

    cmdPipelineBarrier2(cmd, &dep1);

    // ------------------------------------------------------------
    // Copy staging buffer -> image.
    // ------------------------------------------------------------

    VkBufferImageCopy region{};

    region.imageSubresource = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        0,
        1
    };

    region.imageExtent = {
        static_cast<uint32_t>(width_),
        static_cast<uint32_t>(height_),
        1
    };

    vkCmdCopyBufferToImage(
        cmd,
        stagingBuffer_,
        textureImage_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region
    );

    // ------------------------------------------------------------
    // Transition texture -> SHADER_READ_ONLY.
    // ------------------------------------------------------------

    VkImageMemoryBarrier2 toShaderRead{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2
    };

    toShaderRead.srcStageMask =
        VK_PIPELINE_STAGE_2_COPY_BIT;

    toShaderRead.srcAccessMask =
        VK_ACCESS_2_TRANSFER_WRITE_BIT;

    toShaderRead.dstStageMask =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

    toShaderRead.dstAccessMask =
        VK_ACCESS_2_SHADER_READ_BIT;

    toShaderRead.oldLayout =
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    toShaderRead.newLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    toShaderRead.image =
        textureImage_;

    toShaderRead.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        1,
        0,
        1
    };

    VkDependencyInfo dep2{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO
    };

    dep2.imageMemoryBarrierCount = 1;
    dep2.pImageMemoryBarriers = &toShaderRead;

    cmdPipelineBarrier2(cmd, &dep2);

    textureLayoutIsShaderRead_ = true;

    // Ensure the descriptor points at the CPU-backed texture for subsequent frames
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = textureView_;
    imageInfo.sampler = sampler_;

    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(ctx_.device(), 1, &write, 0, nullptr);
}



bool VulkanDisplayStage::process(const Frame& frame)
{
    if (!initialized_)
        return false;

    glfwPollEvents();

    if (frame.empty())
        return shouldContinue();

    // Begin frame
    uint32_t imageIndex = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    if (!ctx_.beginFrame(imageIndex, cmd))
    {
        ctx_.recreateSwapchain();
        return shouldContinue();
    }

    // If the frame carries a dmabuf fd, use the zero-copy GPU demosaic path.
    if (frame.dmabufFd != -1 && demosaicStage_)
    {
        std::cerr << "[mapYPlane] handing duped fd to VulkanDemosaic\n";
        if (!demosaicStage_->processDmabuf(frame.dmabufFd, frame.dmabufWidth, frame.dmabufHeight))
        {
            std::cerr << "[VulkanDisplayStage] VulkanDemosaicStage failed\n";
            return false; // per your instruction: no fallback
        }

        // Update descriptor to point at the demosaic stage's output image view.
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = demosaicStage_->outputImageView();
        imageInfo.sampler = sampler_;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = descriptorSet_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(ctx_.device(), 1, &write, 0, nullptr);

        // Mark texture as ready for shader read.
        textureLayoutIsShaderRead_ = true;
    }
    else
    {
        // Upload camera frame into texture (CPU path)
        uploadFrame(cmd, frame);
    }

    // Get Vulkan KHR function pointers
    auto cmdPipelineBarrier2 = ctx_.cmdPipelineBarrier2();
    auto cmdBeginRendering   = ctx_.cmdBeginRendering();
    auto cmdEndRendering     = ctx_.cmdEndRendering();

    if (!cmdPipelineBarrier2 ||
        !cmdBeginRendering ||
        !cmdEndRendering)
    {
        std::cerr
            << "[process] ERROR: required Vulkan command function "
               "pointer is NULL\n";

        return shouldContinue();
    }

    // Swapchain:
    // UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier2 toColorAttachment{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2
    };

    toColorAttachment.srcStageMask =
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;

    toColorAttachment.srcAccessMask = 0;

    toColorAttachment.dstStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    toColorAttachment.dstAccessMask =
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

    toColorAttachment.oldLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;

    toColorAttachment.newLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    toColorAttachment.image =
        ctx_.swapchainImage(imageIndex);

    toColorAttachment.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        1,
        0,
        1
    };

    VkDependencyInfo depToColor{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO
    };

    depToColor.imageMemoryBarrierCount = 1;
    depToColor.pImageMemoryBarriers = &toColorAttachment;

    cmdPipelineBarrier2(cmd, &depToColor);

    // Begin dynamic rendering
    VkRenderingAttachmentInfo colorAttachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO
    };

    colorAttachment.imageView =
        ctx_.swapchainImageView(imageIndex);

    colorAttachment.imageLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    colorAttachment.loadOp =
        VK_ATTACHMENT_LOAD_OP_CLEAR;

    colorAttachment.storeOp =
        VK_ATTACHMENT_STORE_OP_STORE;

    colorAttachment.clearValue.color = {
        { 0.0f, 0.0f, 0.0f, 1.0f }
    };

    VkRenderingInfo renderingInfo{
        VK_STRUCTURE_TYPE_RENDERING_INFO
    };

    renderingInfo.renderArea = {
        { 0, 0 },
        ctx_.swapchainExtent()
    };

    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    cmdBeginRendering(cmd, &renderingInfo);

    // Viewport and scissor
    VkExtent2D extent = ctx_.swapchainExtent();

    VkViewport viewport{
        0.0f,
        0.0f,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        0.0f,
        1.0f
    };

    VkRect2D scissor{
        { 0, 0 },
        extent
    };

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind graphics pipeline
    vkCmdBindPipeline(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_
    );

    // Bind descriptor set
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout_,
        0,
        1,
        &descriptorSet_,
        0,
        nullptr
    );

    // Draw fullscreen triangle
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // End dynamic rendering
    cmdEndRendering(cmd);

    // Swapchain:
    // COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR
    VkImageMemoryBarrier2 toPresent{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2
    };

    toPresent.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    toPresent.srcAccessMask =
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

    toPresent.dstStageMask =
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;

    toPresent.dstAccessMask = 0;

    toPresent.oldLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    toPresent.newLayout =
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    toPresent.image =
        ctx_.swapchainImage(imageIndex);

    toPresent.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        1,
        0,
        1
    };

    VkDependencyInfo depToPresent{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO
    };

    depToPresent.imageMemoryBarrierCount = 1;
    depToPresent.pImageMemoryBarriers = &toPresent;

    cmdPipelineBarrier2(cmd, &depToPresent);

    // Submit and present
    if (!ctx_.endFrame(imageIndex, cmd))
        ctx_.recreateSwapchain();

    return shouldContinue();
}


bool VulkanDisplayStage::shouldContinue() const
{
    if (!window_) return false;
    return !glfwWindowShouldClose(window_) && glfwGetKey(window_, GLFW_KEY_ESCAPE) != GLFW_PRESS;
}

void VulkanDisplayStage::destroyTextureResources()
{
    VkDevice device = ctx_.device();
    if (stagingMapped_) vkUnmapMemory(device, stagingMemory_);
    if (stagingBuffer_) vkDestroyBuffer(device, stagingBuffer_, nullptr);
    if (stagingMemory_) vkFreeMemory(device, stagingMemory_, nullptr);
    if (sampler_) vkDestroySampler(device, sampler_, nullptr);
    if (textureView_) vkDestroyImageView(device, textureView_, nullptr);
    if (textureImage_) vkDestroyImage(device, textureImage_, nullptr);
    if (textureMemory_) vkFreeMemory(device, textureMemory_, nullptr);
}

void VulkanDisplayStage::shutdown()
{
    if (!initialized_)
        return;

    VkDevice device = ctx_.device();
    vkDeviceWaitIdle(device);

    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);

    destroyTextureResources();

    if (demosaicStage_) {
        delete demosaicStage_;
        demosaicStage_ = nullptr;
    }

    ctx_.shutdown();

    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();

    initialized_ = false;
    window_ = nullptr;
}
