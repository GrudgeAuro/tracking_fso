#pragma once

#include "ICameraSource.h"
#include <libcamera/libcamera.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

// RAW Bayer camera source. No CPU pixel conversion or CPU image copy is
// performed. The Frame returned to the pipeline contains the libcamera
// DMA-BUF descriptor and metadata required by the Vulkan processing stages.
class RaspberryPiCameraSource : public ICameraSource
{
public:
    explicit RaspberryPiCameraSource(std::string cameraIdSubstring = "");
    ~RaspberryPiCameraSource() override;

    bool open(int width, int height, int fps) override;
    bool grabFrame(Frame& outFrame) override;
    void close() override;

    bool isOpen() const override;
    int width() const override;
    int height() const override;
    double fps() const override;
    std::string name() const override;

private:
    void onRequestComplete(libcamera::Request* request);
    bool mapRawPlane(const libcamera::FrameBuffer* buffer,
                     libcamera::Request* request,
                     Frame& outFrame);
    void recycleRequest(libcamera::Request* request);

    std::string cameraIdSubstring_;

    std::unique_ptr<libcamera::CameraManager> cameraManager_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> config_;
    libcamera::Stream* stream_ = nullptr;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::queue<libcamera::Request*> completedRequests_;

    int width_ = 0;
    int height_ = 0;
    unsigned int rawStride_ = 0;
    size_t rawBufferSize_ = 0;
    libcamera::PixelFormat rawPixelFormat_;
    double fps_ = 0.0;
    bool open_ = false;
};
