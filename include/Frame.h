#pragma once

#include <opencv2/core.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <sys/mman.h>

// Frame is the common currency between the camera source and ANY consumer:
// the Vulkan display stage today, and later e.g. an Ethernet/UDP sink that
// packetizes this same buffer for an external FPGA. Nothing in here is
// display- or GPU-specific on purpose.
//
// image is CV_8UC1: the Y (luma) plane only, tightly packed (step == cols).
// We deliberately do NOT carry chroma -- the PRBS tracking work is
// luma-based, and dropping UV halves the data volume before it ever has to
// cross the PCIe/USB/Ethernet boundary to wherever it's consumed next.
//
// For signal processing: consumers needing 16-bit luma should convert
// locally using OpenCV's convertTo() with appropriate scaling.
//
// mmapBase/mmapLength: if set, Frame owns an mmap region that must be munmap'd
// when the frame is destroyed or reassigned. This allows zero-copy access to
// dmabuf-backed camera buffers.
struct Frame
{
    cv::Mat image; // CV_8UC1, Y channel only
    std::chrono::steady_clock::time_point timestamp;
    uint64_t sequenceNumber = 0;
    void* mmapBase = nullptr;
    size_t mmapLength = 0;

    Frame() = default;

    // Destructor unmaps any held mmap region
    ~Frame()
    {
        cleanup();
    }

    // Move constructor: transfer mmap ownership
    Frame(Frame&& other) noexcept
        : image(std::move(other.image)),
          timestamp(std::move(other.timestamp)),
          sequenceNumber(other.sequenceNumber),
          mmapBase(other.mmapBase),
          mmapLength(other.mmapLength)
    {
        other.mmapBase = nullptr;
        other.mmapLength = 0;
    }

    // Move assignment: clean up old mmap, take ownership of new one
    Frame& operator=(Frame&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            image = std::move(other.image);
            timestamp = std::move(other.timestamp);
            sequenceNumber = other.sequenceNumber;
            mmapBase = other.mmapBase;
            mmapLength = other.mmapLength;
            other.mmapBase = nullptr;
            other.mmapLength = 0;
        }
        return *this;
    }

    // Delete copy constructor and assignment: frames are move-only
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    bool empty() const { return image.empty(); }
    int width() const { return image.cols; }
    int height() const { return image.rows; }

private:
    void cleanup()
    {
        if (mmapBase && mmapLength > 0)
        {
            munmap(mmapBase, mmapLength);
            mmapBase = nullptr;
            mmapLength = 0;
        }
    }
};
