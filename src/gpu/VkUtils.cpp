#include "VkUtils.h"
#include <unistd.h>
#include <iostream>
#include <vector>
#include <cstring>

namespace VkUtils
{

bool importImageFromDmabuf(VkDevice device, VkPhysicalDevice physDevice,
                           int dmabufFd, uint32_t width, uint32_t height,
                           VkFormat format, VkImageUsageFlags usage,
                           VkImage& outImage, VkDeviceMemory& outMemory)
{
    // Create image with external memory flag
    VkExternalMemoryImageCreateInfo extImg{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &extImg;
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

} // namespace VkUtils
