#pragma once

#include <opencv2/core.hpp>
#include <chrono>
#include <cstdint>

// Frame is the common currency between the camera source and ANY consumer:
// the Vulkan display stage today, and later e.g. an Ethernet/UDP sink that
// packetizes this same buffer for an external FPGA. Nothing in here is
// display- or GPU-specific on purpose.
//
// image is CV_8UC1: the Y (luma) plane only, tightly packed (step == cols).
// We deliberately do NOT carry chroma -- the PRBS tracking work is
// luma-based, and dropping UV halves the data volume before it ever has to
// cross the PCIe/USB/Ethernet boundary to wherever it's consumed next.
struct Frame
{
    cv::Mat image; // CV_8UC1, Y channel only
    std::chrono::steady_clock::time_point timestamp;
    uint64_t sequenceNumber = 0;

    bool empty() const { return image.empty(); }
    int width() const { return image.cols; }
    int height() const { return image.rows; }
};
