#pragma once

// Raspberry Pi 5 CSI camera capture via libcamera. Requires libcamera-dev
// (apt install libcamera-dev libcamera-tools). Targets the Pi 5's V3D/ISP
// stack specifically -- tested sensors: Raspberry Pi HQ Camera (IMX477) and
// Camera Module 3 NoIR (IMX708).
//
// Captures in YUV420 and hands out ONLY the Y plane -- see mapBuffer(). This
// is not a display-driven crop: it's the native ISP output format, so
// grabbing the Y plane costs nothing extra (no color-conversion pass) and
// halves the bytes that would otherwise need moving for any later
// Ethernet/FPGA sink.

#include "ICameraSource.h"
#include <libcamera/libcamera.h>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>

class RaspberryPiCameraSource : public ICameraSource
{
public:
    // cameraIdSubstring: if non-empty, picks the first detected camera whose
    // libcamera ID contains this substring (useful with two CSI cameras
    // attached, e.g. HQ Cam on cam0 + Camera Module 3 on cam1). Empty picks
    // the first camera libcamera reports.
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
    bool mapYPlane(const libcamera::FrameBuffer* buffer, Frame& outFrame);

    std::string cameraIdSubstring_;

    std::unique_ptr<libcamera::CameraManager> cameraManager_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> config_;
    libcamera::Stream* stream_ = nullptr;

    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;

    // Completed requests are handed from libcamera's internal thread to
    // grabFrame() (called from the main pipeline thread) via this queue.
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::queue<libcamera::Request*> completedRequests_;

    int width_ = 0;
    int height_ = 0;
    unsigned int yStride_ = 0;
    double fps_ = 0.0;
    bool open_ = false;
};
