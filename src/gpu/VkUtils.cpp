#include "VkUtils.h"
#include <unistd.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <fstream>

namespace VkUtils
{

bool importImageFromDmabuf(VkDevice device, VkPhysicalDevice physDevice,
                           int dmabufFd, uint32_t width, uint32_t height,
                           VkFormat format, VkImageUsageFlags usage,
                           VkImage& outImage, VkDeviceMemory& outMemory)
{
    // Create image with external memory flag.
    // Use OPTIMAL tiling for external memory imports.
    VkExternalMemoryImageCreateInfo extImg{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &extImg;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.format = format;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;  // Use OPTIMAL tiling
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.usage = usage;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &ici, nullptr, &outImage) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkCreateImage (dmabuf import) failed\n";
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, outImage, &memReq);

    // Import memory from fd
    VkImportMemoryFdInfoKHR importFd{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    importFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importFd.fd = dmabufFd;

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &importFd;
    mai.allocationSize = memReq.size;
    // Find a memory type index that is compatible with the requirements and device local
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((memReq.memoryTypeBits & (1u << i)))
        {
            chosen = i;
            break;
        }
    }
    if (chosen == UINT32_MAX)
    {
        std::cerr << "[VkUtils] No compatible memory type for imported image\n";
        vkDestroyImage(device, outImage, nullptr);
        return false;
    }

    mai.memoryTypeIndex = chosen;

    if (vkAllocateMemory(device, &mai, nullptr, &outMemory) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkAllocateMemory (import fd) failed\n";
        vkDestroyImage(device, outImage, nullptr);
        return false;
    }

    if (vkBindImageMemory(device, outImage, outMemory, 0) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkBindImageMemory (imported) failed\n";
        vkFreeMemory(device, outMemory, nullptr);
        vkDestroyImage(device, outImage, nullptr);
        return false;
    }

    // Successful import. We dup the fd to avoid the caller losing ownership semantics.
    // (vkAllocateMemory with import consumes the fd on some platforms; to be safe we
    // close the original here — caller should not reuse the fd after calling this.)
    close(dmabufFd);
    std::cerr << "[VkUtils] Imported dmabuf fd into VkImage\n";
    return true;
}

bool createImage2D(VkDevice device, VkPhysicalDevice physDevice,
                   uint32_t width, uint32_t height, VkFormat format,
                   VkImageUsageFlags usage,
                   VkImage& outImage, VkDeviceMemory& outMemory)
{
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.format = format;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.usage = usage;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &ici, nullptr, &outImage) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkCreateImage failed\n";
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, outImage, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            chosen = i;
            break;
        }
    }
    if (chosen == UINT32_MAX)
    {
        std::cerr << "[VkUtils] No device-local memory type available\n";
        vkDestroyImage(device, outImage, nullptr);
        return false;
    }

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = chosen;

    if (vkAllocateMemory(device, &mai, nullptr, &outMemory) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkAllocateMemory failed\n";
        vkDestroyImage(device, outImage, nullptr);
        return false;
    }

    if (vkBindImageMemory(device, outImage, outMemory, 0) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkBindImageMemory failed\n";
        vkFreeMemory(device, outMemory, nullptr);
        vkDestroyImage(device, outImage, nullptr);
        return false;
    }

    return true;
}

VkImageView createImageView2D(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectMask)
{
    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = format;
    ivci.subresourceRange.aspectMask = aspectMask;
    ivci.subresourceRange.baseMipLevel = 0;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &ivci, nullptr, &view) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkCreateImageView failed\n";
        return VK_NULL_HANDLE;
    }
    return view;
}

bool createBuffer(VkDevice device, VkPhysicalDevice physDevice,
                  VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags memProps,
                  VkBuffer& outBuffer, VkDeviceMemory& outMemory)
{
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bci, nullptr, &outBuffer) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkCreateBuffer failed\n";
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, outBuffer, &memReq);

    VkPhysicalDeviceMemoryProperties physMemProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &physMemProps);

    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < physMemProps.memoryTypeCount; ++i)
    {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (physMemProps.memoryTypes[i].propertyFlags & memProps) == memProps)
        {
            chosen = i;
            break;
        }
    }
    if (chosen == UINT32_MAX)
    {
        std::cerr << "[VkUtils] No suitable memory type for buffer\n";
        vkDestroyBuffer(device, outBuffer, nullptr);
        return false;
    }

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = chosen;

    if (vkAllocateMemory(device, &mai, nullptr, &outMemory) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkAllocateMemory (buffer) failed\n";
        vkDestroyBuffer(device, outBuffer, nullptr);
        return false;
    }

    if (vkBindBufferMemory(device, outBuffer, outMemory, 0) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkBindBufferMemory failed\n";
        vkFreeMemory(device, outMemory, nullptr);
        vkDestroyBuffer(device, outBuffer, nullptr);
        return false;
    }

    return true;
}

VkShaderModule loadShaderModule(VkDevice device, const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        std::cerr << "[VkUtils] Failed to open shader file: " << path << "\n";
        return VK_NULL_HANDLE;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();

    VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = fileSize;
    smci.pCode = buffer.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smci, nullptr, &module) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkCreateShaderModule failed for " << path << "\n";
        return VK_NULL_HANDLE;
    }

    return module;
}

} // namespace VkUtils
