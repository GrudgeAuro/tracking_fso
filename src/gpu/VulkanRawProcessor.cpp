#include "VulkanRawProcessor.h"
#include "VkUtils.h"

#include <iostream>
#include <unistd.h>

namespace
{
constexpr VkFormat kProcessFormat = VK_FORMAT_R16_UINT;

struct RawPushConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t strideBytes;
    uint32_t planeOffsetBytes;
};

struct ContrastPushConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t tileCountX;
    uint32_t tileSize;
};
}

bool VulkanRawProcessor::init(VulkanContext& ctx, uint32_t width, uint32_t height)
{
    ctx_ = &ctx;
    device_ = ctx.device();
    width_ = width;
    height_ = height;
    tileCountX_ = (width_ + tileSize_ - 1) / tileSize_;
    tileCountY_ = (height_ + tileSize_ - 1) / tileSize_;

    if (!createImages()) return false;
    if (!createMinMaxBuffer()) return false;
    if (!createDescriptorLayouts()) return false;
    if (!createDescriptorPool()) return false;
    if (!createPipelines()) return false;

    return true;
}

bool VulkanRawProcessor::createImages()
{
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(ctx_->physicalDevice(), kProcessFormat, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
    {
        std::cerr << "[VulkanRawProcessor] VK_FORMAT_R16_UINT is not supported as a storage+sampled image on this GPU\n";
        return false;
    }

    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!VkUtils::createImage2D(device_, ctx_->physicalDevice(), width_, height_,
                                kProcessFormat, usage, monoImage_, monoMemory_))
        return false;
    if (!VkUtils::createImage2D(device_, ctx_->physicalDevice(), width_, height_,
                                kProcessFormat, usage, medianImage_, medianMemory_))
        return false;
    if (!VkUtils::createImage2D(device_, ctx_->physicalDevice(), width_, height_,
                                kProcessFormat, usage, contrastImage_, contrastMemory_))
        return false;

    monoView_ = VkUtils::createImageView2D(device_, monoImage_, kProcessFormat,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
    medianView_ = VkUtils::createImageView2D(device_, medianImage_, kProcessFormat,
                                             VK_IMAGE_ASPECT_COLOR_BIT);
    contrastView_ = VkUtils::createImageView2D(device_, contrastImage_, kProcessFormat,
                                               VK_IMAGE_ASPECT_COLOR_BIT);

    return monoView_ && medianView_ && contrastView_;
}

bool VulkanRawProcessor::createMinMaxBuffer()
{
    const VkDeviceSize count =
        static_cast<VkDeviceSize>(tileCountX_) * tileCountY_;

    if (!VkUtils::createBuffer(
        device_, ctx_->physicalDevice(), count * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, minBuffer_, minMemory_))
        return false;

    return VkUtils::createBuffer(
        device_, ctx_->physicalDevice(), count * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, maxBuffer_, maxMemory_);
}

bool VulkanRawProcessor::createDescriptorLayouts()
{
    // RAW SSBO.
    VkDescriptorSetLayoutBinding rawBinding{};
    rawBinding.binding = 0;
    rawBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    rawBinding.descriptorCount = 1;
    rawBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = 1;
    ci.pBindings = &rawBinding;
    if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &rawLayout_) != VK_SUCCESS)
        return false;

    // image -> image.
    VkDescriptorSetLayoutBinding imageBindings[2]{};
    imageBindings[0].binding = 0;
    imageBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    imageBindings[0].descriptorCount = 1;
    imageBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    imageBindings[1] = imageBindings[0];
    imageBindings[1].binding = 1;

    ci.bindingCount = 2;
    ci.pBindings = imageBindings;
    if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &imageToImageLayout_) != VK_SUCCESS)
        return false;

    // image + min + max buffers.
    VkDescriptorSetLayoutBinding minmaxBindings[3]{};
    minmaxBindings[0] = imageBindings[0];
    minmaxBindings[0].binding = 0;
    minmaxBindings[1].binding = 1;
    minmaxBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    minmaxBindings[1].descriptorCount = 1;
    minmaxBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    minmaxBindings[2] = minmaxBindings[1];
    minmaxBindings[2].binding = 2;

    ci.bindingCount = 3;
    ci.pBindings = minmaxBindings;
    if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &minMaxLayout_) != VK_SUCCESS)
        return false;

    // image + min + max buffers + output image.
    VkDescriptorSetLayoutBinding contrastBindings[4]{};
    contrastBindings[0] = minmaxBindings[0];
    contrastBindings[0].binding = 0;
    contrastBindings[1] = minmaxBindings[1];
    contrastBindings[2] = minmaxBindings[2];
    contrastBindings[3] = minmaxBindings[0];
    contrastBindings[3].binding = 3;

    ci.bindingCount = 4;
    ci.pBindings = contrastBindings;
    return vkCreateDescriptorSetLayout(device_, &ci, nullptr, &contrastLayout_) == VK_SUCCESS;
}

bool VulkanRawProcessor::createDescriptorPool()
{
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10 }
    };

    VkDescriptorPoolCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    ci.maxSets = 4;
    ci.poolSizeCount = 2;
    ci.pPoolSizes = sizes;
    return vkCreateDescriptorPool(device_, &ci, nullptr, &descriptorPool_) == VK_SUCCESS;
}

bool VulkanRawProcessor::createPipelines()
{
    VkShaderModule rawModule = VkUtils::loadShaderModule(device_, "shaders/bayer_to_mono.comp.spv");
    VkShaderModule medianModule = VkUtils::loadShaderModule(device_, "shaders/median_filter.comp.spv");
    VkShaderModule minmaxModule = VkUtils::loadShaderModule(device_, "shaders/local_minmax.comp.spv");
    VkShaderModule contrastModule = VkUtils::loadShaderModule(device_, "shaders/local_contrast.comp.spv");

    if (!rawModule || !medianModule || !minmaxModule || !contrastModule)
    {
        std::cerr << "[VulkanRawProcessor] Failed to load compute shaders\n";
        return false;
    }

    VkDescriptorSetLayout layouts[] = {
        rawLayout_, imageToImageLayout_, minMaxLayout_, contrastLayout_
    };
    VkPipelineLayout* pipelineLayouts[] = {
        &rawPipelineLayout_, &medianPipelineLayout_,
        &minMaxPipelineLayout_, &contrastPipelineLayout_
    };
    VkShaderModule modules[] = { rawModule, medianModule, minmaxModule, contrastModule };

    for (int i = 0; i < 4; ++i)
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        range.offset = 0;
        range.size = (i == 3) ? sizeof(ContrastPushConstants) : sizeof(RawPushConstants);

        VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &layouts[i];
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(device_, &lci, nullptr, pipelineLayouts[i]) != VK_SUCCESS)
            return false;

        VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = modules[i];
        stage.pName = "main";

        VkComputePipelineCreateInfo pci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pci.stage = stage;
        pci.layout = *pipelineLayouts[i];

        VkPipeline* pipeline = nullptr;
        switch (i)
        {
        case 0: pipeline = &rawPipeline_; break;
        case 1: pipeline = &medianPipeline_; break;
        case 2: pipeline = &minMaxPipeline_; break;
        case 3: pipeline = &contrastPipeline_; break;
        }

        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, pipeline) != VK_SUCCESS)
            return false;
    }

    vkDestroyShaderModule(device_, rawModule, nullptr);
    vkDestroyShaderModule(device_, medianModule, nullptr);
    vkDestroyShaderModule(device_, minmaxModule, nullptr);
    vkDestroyShaderModule(device_, contrastModule, nullptr);

    // Allocate descriptors.
    VkDescriptorSetLayout setLayouts[] = {
        rawLayout_, imageToImageLayout_, minMaxLayout_, contrastLayout_
    };

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = descriptorPool_;
    ai.descriptorSetCount = 1;

    ai.pSetLayouts = &setLayouts[0];
    if (vkAllocateDescriptorSets(device_, &ai, &rawSet_) != VK_SUCCESS) return false;
    ai.pSetLayouts = &setLayouts[1];
    if (vkAllocateDescriptorSets(device_, &ai, &medianSet_) != VK_SUCCESS) return false;
    ai.pSetLayouts = &setLayouts[2];
    if (vkAllocateDescriptorSets(device_, &ai, &minMaxSet_) != VK_SUCCESS) return false;
    ai.pSetLayouts = &setLayouts[3];
    if (vkAllocateDescriptorSets(device_, &ai, &contrastSet_) != VK_SUCCESS) return false;

    // Fixed image descriptors.
    VkDescriptorImageInfo monoInfo{ VK_NULL_HANDLE, monoView_, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo medianInfo{ VK_NULL_HANDLE, medianView_, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo contrastInfo{ VK_NULL_HANDLE, contrastView_, VK_IMAGE_LAYOUT_GENERAL };

    VkDescriptorBufferInfo minInfo{ minBuffer_, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo maxInfo{ maxBuffer_, 0, VK_WHOLE_SIZE };

    VkWriteDescriptorSet mmWrites[3]{};
    mmWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    mmWrites[0].dstSet = minMaxSet_;
    mmWrites[0].dstBinding = 0;
    mmWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    mmWrites[0].descriptorCount = 1;
    mmWrites[0].pImageInfo = &medianInfo;
    mmWrites[1] = mmWrites[0];
    mmWrites[1].dstBinding = 1;
    mmWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    mmWrites[1].pImageInfo = nullptr;
    mmWrites[1].pBufferInfo = &minInfo;
    mmWrites[2] = mmWrites[1];
    mmWrites[2].dstBinding = 2;
    mmWrites[2].pBufferInfo = &maxInfo;
    vkUpdateDescriptorSets(device_, 3, mmWrites, 0, nullptr);

    VkDescriptorImageInfo contrastInputs[2] = { medianInfo, contrastInfo };
    VkWriteDescriptorSet contrastWrites[4]{};
    contrastWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    contrastWrites[0].dstSet = contrastSet_;
    contrastWrites[0].dstBinding = 0;
    contrastWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    contrastWrites[0].descriptorCount = 1;
    contrastWrites[0].pImageInfo = &contrastInputs[0];
    contrastWrites[1] = contrastWrites[0];
    contrastWrites[1].dstBinding = 1;
    contrastWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    contrastWrites[1].pImageInfo = nullptr;
    contrastWrites[1].pBufferInfo = &minInfo;
    contrastWrites[2] = contrastWrites[1];
    contrastWrites[2].dstBinding = 2;
    contrastWrites[2].pBufferInfo = &maxInfo;
    contrastWrites[3] = contrastWrites[0];
    contrastWrites[3].dstBinding = 3;
    contrastWrites[3].pImageInfo = &contrastInputs[1];
    vkUpdateDescriptorSets(device_, 4, contrastWrites, 0, nullptr);

    return true;
}

bool VulkanRawProcessor::importDmaBuf(const Frame& frame, ImportedBuffer& out)
{
    int dupFd = ::dup(frame.dmabufFd);
    if (dupFd < 0)
    {
        std::cerr << "[VulkanRawProcessor] dup(DMA-BUF) failed\n";
        return false;
    }

    VkExternalMemoryBufferCreateInfo externalInfo{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO
    };
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.pNext = &externalInfo;
    bci.size = frame.bufferSize;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bci, nullptr, &out.buffer) != VK_SUCCESS)
    {
        ::close(dupFd);
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, out.buffer, &req);

    VkMemoryFdPropertiesKHR fdProps{
        VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR
    };
    
    
    PFN_vkGetMemoryFdPropertiesKHR pfnGetMemoryFdPropertiesKHR =
    reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));

    if (!pfnGetMemoryFdPropertiesKHR)
    {
        std::cerr << "[VulkanRawProcessor] "
                     "vkGetMemoryFdPropertiesKHR not available\n";
    
        ::close(dupFd);
        vkDestroyBuffer(device_, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    
    VkResult result = pfnGetMemoryFdPropertiesKHR(
        device_,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        dupFd,
        &fdProps);


    if (result != VK_SUCCESS)
    {
        ::close(dupFd);
        vkDestroyBuffer(device_, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    uint32_t memoryTypeIndex = VkUtils::findMemoryType(
        ctx_->physicalDevice(), fdProps.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (memoryTypeIndex == UINT32_MAX)
    {
        ::close(dupFd);
        vkDestroyBuffer(device_, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    VkImportMemoryFdInfoKHR importInfo{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR
    };
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd = dupFd;

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &importInfo;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryTypeIndex;

    result = vkAllocateMemory(device_, &mai, nullptr, &out.memory);
    if (result != VK_SUCCESS)
    {
        // On failure Vulkan does not take ownership of the fd.
        ::close(dupFd);
        vkDestroyBuffer(device_, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindBufferMemory(device_, out.buffer, out.memory, 0);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device_, out.memory, nullptr);
        vkDestroyBuffer(device_, out.buffer, nullptr);
        out.memory = VK_NULL_HANDLE;
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    out.size = frame.bufferSize;
    return true;
}

bool VulkanRawProcessor::getImportedBuffer(const Frame& frame, VkBuffer& outBuffer)
{
    auto it = importedBuffers_.find(frame.dmabufFd);
    if (it == importedBuffers_.end())
    {
        ImportedBuffer resource;
        if (!importDmaBuf(frame, resource))
        {
            std::cerr << "[VulkanRawProcessor] Failed to import camera DMA-BUF\n";
            return false;
        }
        it = importedBuffers_.emplace(frame.dmabufFd, resource).first;
    }

    outBuffer = it->second.buffer;
    return true;
}

void VulkanRawProcessor::transitionImage(
    VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    ctx_->cmdPipelineBarrier2()(cmd, &dep);
}

bool VulkanRawProcessor::record(VkCommandBuffer cmd, const Frame& frame)
{
    VkBuffer rawBuffer = VK_NULL_HANDLE;
    if (!getImportedBuffer(frame, rawBuffer))
        return false;

    auto barrier2 = ctx_->cmdPipelineBarrier2();
    if (!barrier2)
        return false;

    // The contrast image is presented in SHADER_READ_ONLY_OPTIMAL after each
    // frame. Bring it back to GENERAL before overwriting it on the next frame.
    if (contrastShaderRead_)
    {
        transitionImage(cmd, contrastImage_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    // All processing images start undefined on the first frame.
    if (!monoShaderRead_)
    {
        transitionImage(cmd, monoImage_, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT);
        monoShaderRead_ = true;
    }
    if (!medianShaderRead_)
    {
        transitionImage(cmd, medianImage_, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT);
        medianShaderRead_ = true;
    }
    if (!contrastShaderRead_)
    {
        transitionImage(cmd, contrastImage_, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT);
        contrastShaderRead_ = true;
    }

    // RAW descriptor is rewritten because the camera DMA-BUF changes.
    VkDescriptorBufferInfo rawInfo{ rawBuffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet rawWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    rawWrite.dstSet = rawSet_;
    rawWrite.dstBinding = 0;
    rawWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    rawWrite.descriptorCount = 1;
    rawWrite.pBufferInfo = &rawInfo;
    vkUpdateDescriptorSets(device_, 1, &rawWrite, 0, nullptr);

    // 1. Bayer -> monochrome.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rawPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            rawPipelineLayout_, 0, 1, &rawSet_, 0, nullptr);
    RawPushConstants pc{ width_, height_, frame.strideBytes, frame.planeOffset };
    vkCmdPushConstants(cmd, rawPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (width_ + 15) / 16, (height_ + 15) / 16, 1);

    // mono write -> median read.
    transitionImage(cmd, monoImage_, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);

    // 2. Median filter.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, medianPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            medianPipelineLayout_, 0, 1, &medianSet_, 0, nullptr);
    vkCmdPushConstants(cmd, medianPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (width_ + 15) / 16, (height_ + 15) / 16, 1);

    transitionImage(cmd, medianImage_, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);

    // Initialize min/max entirely on the GPU.
    const VkDeviceSize tileBytes =
        static_cast<VkDeviceSize>(tileCountX_) * tileCountY_ * sizeof(uint32_t);
    vkCmdFillBuffer(cmd, minBuffer_, 0, tileBytes, 0xFFFFFFFFu);
    vkCmdFillBuffer(cmd, maxBuffer_, 0, tileBytes, 0u);

    VkBufferMemoryBarrier2 fillBarriers[2]{};
    for (int i = 0; i < 2; ++i)
    {
        fillBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        fillBarriers[i].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        fillBarriers[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        fillBarriers[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        fillBarriers[i].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        fillBarriers[i].buffer = (i == 0) ? minBuffer_ : maxBuffer_;
        fillBarriers[i].offset = 0;
        fillBarriers[i].size = VK_WHOLE_SIZE;
    }
    VkDependencyInfo fillDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    fillDep.bufferMemoryBarrierCount = 2;
    fillDep.pBufferMemoryBarriers = fillBarriers;
    barrier2(cmd, &fillDep);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, minMaxPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            minMaxPipelineLayout_, 0, 1, &minMaxSet_, 0, nullptr);
    vkCmdPushConstants(cmd, minMaxPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (width_ + 15) / 16, (height_ + 15) / 16, 1);

    VkBufferMemoryBarrier2 mmBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    mmBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mmBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mmBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mmBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkBufferMemoryBarrier2 mmBarriers[2]{};
    mmBarriers[0] = mmBarrier;
    mmBarriers[0].buffer = minBuffer_;
    mmBarriers[0].size = VK_WHOLE_SIZE;
    mmBarriers[1] = mmBarrier;
    mmBarriers[1].buffer = maxBuffer_;
    mmBarriers[1].size = VK_WHOLE_SIZE;

    VkDependencyInfo mmDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    mmDep.bufferMemoryBarrierCount = 2;
    mmDep.pBufferMemoryBarriers = mmBarriers;
    barrier2(cmd, &mmDep);

    // 3. Local contrast.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, contrastPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            contrastPipelineLayout_, 0, 1, &contrastSet_, 0, nullptr);
    ContrastPushConstants cpc{ width_, height_, tileCountX_, tileSize_ };
    vkCmdPushConstants(cmd, contrastPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(cpc), &cpc);
    vkCmdDispatch(cmd, (width_ + 15) / 16, (height_ + 15) / 16, 1);

    transitionImage(cmd, contrastImage_, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);

    return true;
}

void VulkanRawProcessor::destroyImportedBuffers()
{
    for (auto& [fd, resource] : importedBuffers_)
    {
        if (resource.buffer)
            vkDestroyBuffer(device_, resource.buffer, nullptr);
        if (resource.memory)
            vkFreeMemory(device_, resource.memory, nullptr);
    }
    importedBuffers_.clear();
}

void VulkanRawProcessor::destroyImages()
{
    if (monoView_) vkDestroyImageView(device_, monoView_, nullptr);
    if (medianView_) vkDestroyImageView(device_, medianView_, nullptr);
    if (contrastView_) vkDestroyImageView(device_, contrastView_, nullptr);
    if (monoImage_) vkDestroyImage(device_, monoImage_, nullptr);
    if (medianImage_) vkDestroyImage(device_, medianImage_, nullptr);
    if (contrastImage_) vkDestroyImage(device_, contrastImage_, nullptr);
    if (monoMemory_) vkFreeMemory(device_, monoMemory_, nullptr);
    if (medianMemory_) vkFreeMemory(device_, medianMemory_, nullptr);
    if (contrastMemory_) vkFreeMemory(device_, contrastMemory_, nullptr);

    monoView_ = medianView_ = contrastView_ = VK_NULL_HANDLE;
    monoImage_ = medianImage_ = contrastImage_ = VK_NULL_HANDLE;
    monoMemory_ = medianMemory_ = contrastMemory_ = VK_NULL_HANDLE;
}

void VulkanRawProcessor::destroyPipelines()
{
    if (rawPipeline_) vkDestroyPipeline(device_, rawPipeline_, nullptr);
    if (medianPipeline_) vkDestroyPipeline(device_, medianPipeline_, nullptr);
    if (minMaxPipeline_) vkDestroyPipeline(device_, minMaxPipeline_, nullptr);
    if (contrastPipeline_) vkDestroyPipeline(device_, contrastPipeline_, nullptr);
    if (rawPipelineLayout_) vkDestroyPipelineLayout(device_, rawPipelineLayout_, nullptr);
    if (medianPipelineLayout_) vkDestroyPipelineLayout(device_, medianPipelineLayout_, nullptr);
    if (minMaxPipelineLayout_) vkDestroyPipelineLayout(device_, minMaxPipelineLayout_, nullptr);
    if (contrastPipelineLayout_) vkDestroyPipelineLayout(device_, contrastPipelineLayout_, nullptr);
    rawPipeline_ = medianPipeline_ = minMaxPipeline_ = contrastPipeline_ = VK_NULL_HANDLE;
    rawPipelineLayout_ = medianPipelineLayout_ = minMaxPipelineLayout_ = contrastPipelineLayout_ = VK_NULL_HANDLE;
}

void VulkanRawProcessor::shutdown()
{
    if (!device_)
        return;

    vkDeviceWaitIdle(device_);

    destroyImportedBuffers();
    if (minBuffer_) vkDestroyBuffer(device_, minBuffer_, nullptr);
    if (minMemory_) vkFreeMemory(device_, minMemory_, nullptr);
    if (maxBuffer_) vkDestroyBuffer(device_, maxBuffer_, nullptr);
    if (maxMemory_) vkFreeMemory(device_, maxMemory_, nullptr);

    destroyPipelines();

    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (rawLayout_) vkDestroyDescriptorSetLayout(device_, rawLayout_, nullptr);
    if (imageToImageLayout_) vkDestroyDescriptorSetLayout(device_, imageToImageLayout_, nullptr);
    if (minMaxLayout_) vkDestroyDescriptorSetLayout(device_, minMaxLayout_, nullptr);
    if (contrastLayout_) vkDestroyDescriptorSetLayout(device_, contrastLayout_, nullptr);

    destroyImages();

    minBuffer_ = VK_NULL_HANDLE;
    minMemory_ = VK_NULL_HANDLE;
    maxBuffer_ = VK_NULL_HANDLE;
    maxMemory_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    rawLayout_ = imageToImageLayout_ = minMaxLayout_ = contrastLayout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}
