#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

// A captured camera frame is a descriptor for a camera-owned DMA-BUF.
// Pixel data never passes through a CPU image container.
struct Frame
{
    int dmabufFd = -1;
    uint32_t bufferSize = 0;
    uint32_t planeOffset = 0;
    uint32_t planeLength = 0;
    uint32_t strideBytes = 0;
    uint32_t widthPixels = 0;
    uint32_t heightPixels = 0;

    std::chrono::steady_clock::time_point timestamp;
    uint64_t sequenceNumber = 0;

    // Called after the Vulkan submission using this frame has completed.
    std::shared_ptr<std::function<void()>> releaseCallback;

    bool empty() const { return dmabufFd < 0; }
    int width() const { return static_cast<int>(widthPixels); }
    int height() const { return static_cast<int>(heightPixels); }

    void release()
    {
        if (releaseCallback && *releaseCallback)
        {
            auto cb = std::move(*releaseCallback);
            releaseCallback.reset();
            cb();
        }
    }
};
