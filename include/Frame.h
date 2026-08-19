#pragma once

#include <opencv2/core.hpp>
#include <chrono>
#include <cstdint>
#include <vector>

// Frame is the common currency between the camera source and ANY consumer:
// the Vulkan display stage today, and later e.g. an Ethernet/UDP sink that
// packetizes this same buffer for an external FPGA. Nothing in here is
// display- or GPU-specific on purpose.
//
// image is CV_8UC1 or CV_16UC1: the Y (luma) plane only, tightly packed (step == cols).
// We deliberately do NOT carry chroma -- the PRBS tracking work is
// luma-based, and dropping UV halves the data volume before it ever has to
// cross the PCIe/USB/Ethernet boundary to wherever it's consumed next.
struct Frame
{
    cv::Mat image; // CV_8UC1 or CV_16UC1, Y channel only
    std::chrono::steady_clock::time_point timestamp;
    uint64_t sequenceNumber = 0;

    // For GPU-accelerated demosaic via staging buffer:
    // stagingBuffer holds raw 16-bit data that the Vulkan demosaic compute
    // shader will process. When present, `image` will be empty and the display
    // should upload this buffer to a GPU texture and run the demosaic shader.
    std::vector<uint8_t> stagingBuffer;
    uint32_t stagingWidth = 0;
    uint32_t stagingHeight = 0;

    // Legacy zero-copy DMABUF path (deprecated; dmabuf import not supported on Pi 5).
    int dmabufFd = -1;
    uint32_t dmabufWidth = 0;
    uint32_t dmabufHeight = 0;

    bool empty() const { return image.empty() && dmabufFd == -1 && stagingBuffer.empty(); }
    int width() const { 
        if (!stagingBuffer.empty()) return static_cast<int>(stagingWidth);
        if (image.empty()) return static_cast<int>(dmabufWidth);
        return image.cols;
    }
    int height() const { 
        if (!stagingBuffer.empty()) return static_cast<int>(stagingHeight);
        if (image.empty()) return static_cast<int>(dmabufHeight);
        return image.rows;
    }
};
