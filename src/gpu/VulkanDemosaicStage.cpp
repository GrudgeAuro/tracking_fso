#include "VulkanDemosaicStage.h"
#include "VkUtils.h"
#include "VkUtils.h"
#include <iostream>
#include <chrono>
#include <vector>

// For simplicity we assume the SPIR-V for demosaic.comp is available at
// "shaders/demosaic.comp.spv" relative to the working directory (CMake
// compiles it there).

VulkanDemosaicStage::VulkanDemosaicStage(VkDevice device, VkPhysicalDevice phys, VkCommandPool cmdPool, VkQueue queue)
    : device_(device), phys_(phys), cmdPool_(cmdPool), queue_(queue)
{
    createPipeline();
}

VulkanDemosaicStage::~VulkanDemosaicStage()
{
    destroyPipeline();
    if (outView_) vkDestroyImageView(device_, outView_, nullptr);
    if (outImage_) vkDestroyImage(device_, outImage_, nullptr);
    if (outMemory_) vkFreeMemory(device_, outMemory_, nullptr);
    if (inImage_) vkDestroyImage(device_, inImage_, nullptr);
    if (inMemory_) vkFreeMemory(device_, inMemory_, nullptr);
}

bool VulkanDemosaicStage::importInput(int fd, uint32_t width, uint32_t height)
{
    // Expect input to be VK_FORMAT_R16_UINT (raw 16-bit container)
    if (!VkUtils::importImageFromDmabuf(device_, phys_, fd, width, height, VK_FORMAT_R16_UINT,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                        inImage_, inMemory_))
    {
        std::cerr << "[VulkanDemosaic] importImageFromDmabuf failed\n";
        return false;
    }

    return true;
}

bool VulkanDemosaicStage::createOutput(uint32_t width, uint32_t height)
{
    // Create device-local R16 output image (storage image)
    if (!VkUtils::createImage2D(device_, phys_, width, height, VK_FORMAT_R16_UINT,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                outImage_, outMemory_))
    {
        std::cerr << "[VulkanDemosaic] createImage2D for output failed\n";
        return false;
    }

    outView_ = VkUtils::createImageView2D(device_, outImage_, VK_FORMAT_R16_UINT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (outView_ == VK_NULL_HANDLE)
    {
        std::cerr << "[VulkanDemosaic] createImageView2D failed\n";
        return false;
    }

    return true;
}

bool VulkanDemosaicStage::createPipeline()
{
    // Create descriptor set layout: binding 0 = input (readonly), binding 1 = output (writeonly storage)
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dsli{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dsli.bindingCount = 2;
    dsli.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &dsli, nullptr, &descLayout_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDemosaic] vkCreateDescriptorSetLayout failed\n";
        return false;
    }

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &descLayout_;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDemosaic] vkCreatePipelineLayout failed\n";
        return false;
    }

    // Load compute shader module
    VkShaderModule comp = VkUtils::loadShaderModule(device_, "shaders/demosaic.comp.spv");
    if (!comp)
    {
        std::cerr << "[VulkanDemosaic] Failed to load demosaic shader module\n";
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = comp;
    stage.pName = "main";

    VkComputePipelineCreateInfo pci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pci.stage = stage;
    pci.layout = pipelineLayout_;

    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &computePipeline_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDemosaic] vkCreateComputePipelines failed\n";
        vkDestroyShaderModule(device_, comp, nullptr);
        return false;
    }

    vkDestroyShaderModule(device_, comp, nullptr);

    // Descriptor pool and allocate set
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 2;

    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDemosaic] vkCreateDescriptorPool failed\n";
        return false;
    }

    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = descPool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &descLayout_;
    if (vkAllocateDescriptorSets(device_, &dsai, &descSet_) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDemosaic] vkAllocateDescriptorSets failed\n";
        return false;
    }

    return true;
}

void VulkanDemosaicStage::destroyPipeline()
{
    if (computePipeline_) vkDestroyPipeline(device_, computePipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descPool_) vkDestroyDescriptorPool(device_, descPool_, nullptr);
    if (descLayout_) vkDestroyDescriptorSetLayout(device_, descLayout_, nullptr);
}

bool VulkanDemosaicStage::processDmabuf(int dmabufFd, uint32_t width, uint32_t height)
{
    if (!importInput(dmabufFd, width, height))
        return false;

    if (!createOutput(width, height))
        return false;

    // Update descriptor set to point to input and output images
    VkDescriptorImageInfo inInfo{};
    inInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    // Need to create an image view for inImage
    VkImageView inView = VkUtils::createImageView2D(device_, inImage_, VK_FORMAT_R16_UINT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (inView == VK_NULL_HANDLE)
    {
        std::cerr << "[VulkanDemosaic] createImageView2D for input failed\n";
        return false;
    }

    VkDescriptorImageInfo outInfo{};
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    outInfo.imageView = outView_;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descSet_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &inInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descSet_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outInfo;

    // Note: inInfo.imageView must be set now
    inInfo.imageView = inView;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    // Create command buffer, dispatch compute shader
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDemosaic] vkAllocateCommandBuffers failed\n";
        vkDestroyImageView(device_, inView, nullptr);
        return false;
    }

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    // Transition images to GENERAL layout for compute read/write
    VkImageMemoryBarrier barrierIn{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrierIn.srcAccessMask = 0;
    barrierIn.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrierIn.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrierIn.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrierIn.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrierIn.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrierIn.image = inImage_;
    barrierIn.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier barrierOut = barrierIn;
    barrierOut.image = outImage_;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, (VkImageMemoryBarrier[]){ barrierIn, barrierOut });

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descSet_, 0, nullptr);

    uint32_t groupX = (width + 15) / 16;
    uint32_t groupY = (height + 15) / 16;

    auto t0 = std::chrono::steady_clock::now();
    vkCmdDispatch(cmd, groupX, groupY, 1);

    // Barrier to ensure compute writes are available for shader read (display)
    VkImageMemoryBarrier postBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    postBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    postBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    postBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    postBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    postBarrier.image = outImage_;
    postBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &postBarrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    if (vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        std::cerr << "[VulkanDemosaic] vkQueueSubmit failed\n";
        vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);
        vkDestroyImageView(device_, inView, nullptr);
        return false;
    }

    vkQueueWaitIdle(queue_);

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cerr << "[VulkanDemosaic] dispatched compute demosaic (" << groupX << "x" << groupY << ") took " << ms << " ms\n";

    // Cleanup input view (input image and memory remain bound and owned)
    vkDestroyImageView(device_, inView, nullptr);

    vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);

    return true;
}
