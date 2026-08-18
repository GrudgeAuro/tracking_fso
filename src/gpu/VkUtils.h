#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

// Small grab-bag of Vulkan boilerplate shared by every GPU stage (today:
// VulkanDisplayStage's texture + staging buffer; later: the PRBS
// correlator's SSBOs). Keeping it here instead of duplicating per-stage.
namespace VkUtils
{

uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

bool createBuffer(VkDevice device, VkPhysicalDevice physDevice, VkDeviceSize size,
                   VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                   VkBuffer& outBuffer, VkDeviceMemory& outMemory);

bool createImage2D(VkDevice device, VkPhysicalDevice physDevice, uint32_t width, uint32_t height,
                    VkFormat format, VkImageUsageFlags usage,
                    VkImage& outImage, VkDeviceMemory& outMemory);

VkImageView createImageView2D(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect);

// Loads a compiled .spv file (see CMakeLists.txt shader build step) and
// creates a VkShaderModule from it.
VkShaderModule loadShaderModule(VkDevice device, const std::string& spirvPath);

} // namespace VkUtils
