#include "VkUtils.h"
#include <fstream>
#include <iostream>

namespace VkUtils
{

uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    std::cerr << "[VkUtils] No suitable memory type found\n";
    return UINT32_MAX;
}

bool createBuffer(VkDevice device, VkPhysicalDevice physDevice, VkDeviceSize size,
                   VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
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

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = findMemoryType(physDevice, memReq.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &mai, nullptr, &outMemory) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkAllocateMemory (buffer) failed\n";
        return false;
    }

    vkBindBufferMemory(device, outBuffer, outMemory, 0);
    return true;
}

bool createImage2D(VkDevice device, VkPhysicalDevice physDevice, uint32_t width, uint32_t height,
                    VkFormat format, VkImageUsageFlags usage,
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

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = findMemoryType(physDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &mai, nullptr, &outMemory) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkAllocateMemory (image) failed\n";
        return false;
    }

    vkBindImageMemory(device, outImage, outMemory, 0);
    return true;
}

VkImageView createImageView2D(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect)
{
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = { aspect, 0, 1, 0, 1 };

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &vci, nullptr, &view) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkCreateImageView failed\n";
        return VK_NULL_HANDLE;
    }
    return view;
}

VkShaderModule loadShaderModule(VkDevice device, const std::string& spirvPath)
{
    std::ifstream file(spirvPath, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        std::cerr << "[VkUtils] Failed to open SPIR-V file: " << spirvPath << "\n";
        return VK_NULL_HANDLE;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    file.close();

    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = buffer.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS)
    {
        std::cerr << "[VkUtils] vkCreateShaderModule failed for " << spirvPath << "\n";
        return VK_NULL_HANDLE;
    }
    return module;
}

} // namespace VkUtils
