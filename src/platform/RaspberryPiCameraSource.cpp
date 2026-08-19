#include "RaspberryPiCameraSource.h"
#include <iostream>
#include <algorithm>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <chrono>

using namespace libcamera;

RaspberryPiCameraSource::RaspberryPiCameraSource(std::string cameraIdSubstring)
    : cameraIdSubstring_(std::move(cameraIdSubstring))
{
}

RaspberryPiCameraSource::~RaspberryPiCameraSource()
{
    close();
}

bool RaspberryPiCameraSource::open(int width, int height, int fps)
{
    cameraManager_ = std::make_unique<CameraManager>();
    if (cameraManager_->start())
    {
        std::cerr << "[RaspberryPiCameraSource] Failed to start CameraManager\n";
        return false;
    }

    const auto& cameras = cameraManager_->cameras();
    if (cameras.empty())
    {
        std::cerr << "[RaspberryPiCameraSource] No cameras detected. "
                     "Check `libcamera-hello --list-cameras` on this Pi.\n";
        return false;
    }

    std::cout << "[RaspberryPiCameraSource] Detected cameras:\n";
    for (const auto& cam : cameras)
        std::cout << "    " << cam->id() << "\n";

    if (!cameraIdSubstring_.empty())
    {
        auto it = std::find_if(cameras.begin(), cameras.end(), [this](const auto& cam) {
            return cam->id().find(cameraIdSubstring_) != std::string::npos;
        });
        if (it == cameras.end())
        {
            std::cerr << "[RaspberryPiCameraSource] No camera ID contains '"
                      << cameraIdSubstring_ << "'\n";
            return false;
        }
        camera_ = *it;
    }
    else
    {
        camera_ = cameras.front();
    }

    if (camera_->acquire())
    {
        std::cerr << "[RaspberryPiCameraSource] Failed to acquire camera '" << camera_->id() << "'\n";
        return false;
    }

    // Use VideoRecording role to get YUV420 (or similar) with Y plane
    config_ = camera_->generateConfiguration({ StreamRole::VideoRecording });
    if (!config_ || config_->empty())
    {
        std::cerr << "[RaspberryPiCameraSource] Failed to generate configuration\n";
        return false;
    }

    StreamConfiguration& streamConfig = config_->at(0);
    streamConfig.size.width = width;
    streamConfig.size.height = height;

    CameraConfiguration::Status status = config_->validate();
    if (status == CameraConfiguration::Invalid)
    {
        std::cerr << "[RaspberryPiCameraSource] Invalid configuration\n";
        return false;
    }
    if (status == CameraConfiguration::Adjusted)
    {
        std::cout << "[RaspberryPiCameraSource] Requested " << width << "x" << height
                   << " adjusted by libcamera to " << streamConfig.size.width
                   << "x" << streamConfig.size.height << " (nearest sensor mode)\n";
    }

    if (camera_->configure(config_.get()))
    {
        std::cerr << "[RaspberryPiCameraSource] configure() failed\n";
        return false;
    }

    width_ = static_cast<int>(streamConfig.size.width);
    height_ = static_cast<int>(streamConfig.size.height);
    yStride_ = streamConfig.stride; // row stride of plane 0 (Y plane), in bytes
    stream_ = streamConfig.stream();

    std::cout << "[RaspberryPiCameraSource] Format: " << streamConfig.pixelFormat.toString()
              << ", Y stride: " << yStride_ << " bytes\n";

    allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
    if (allocator_->allocate(stream_) < 0)
    {
        std::cerr << "[RaspberryPiCameraSource] Buffer allocation failed\n";
        return false;
    }

    const auto& buffers = allocator_->buffers(stream_);
    for (const auto& buffer : buffers)
    {
        std::unique_ptr<Request> request = camera_->createRequest();
        if (!request)
        {
            std::cerr << "[RaspberryPiCameraSource] createRequest() failed\n";
            return false;
        }
        if (request->addBuffer(stream_, buffer.get()))
        {
            std::cerr << "[RaspberryPiCameraSource] addBuffer() failed\n";
            return false;
        }
        requests_.push_back(std::move(request));
    }

    camera_->requestCompleted.connect(this, &RaspberryPiCameraSource::onRequestComplete);

    ControlList controls;
    int64_t frameDurationUs;
    if (fps > 0)
    {
        frameDurationUs = 1'000'000 / fps;
    }
    else
    {
        // fps == 0: "max" -- ask the sensor mode we just configured for its
        // fastest achievable frame duration.
        auto it = camera_->controls().find(&controls::FrameDurationLimits);
        if (it != camera_->controls().end())
        {
            frameDurationUs = it->second.min().get<int64_t>();
        }
        else
        {
            std::cerr << "[RaspberryPiCameraSource] FrameDurationLimits not reported; "
                         "falling back to 30 FPS\n";
            frameDurationUs = 1'000'000 / 30;
        }
    }
    controls.set(controls::FrameDurationLimits,
                 Span<const int64_t, 2>({ frameDurationUs, frameDurationUs }));
    fps_ = 1'000'000.0 / static_cast<double>(frameDurationUs);

    if (camera_->start(&controls))
    {
        std::cerr << "[RaspberryPiCameraSource] camera start() failed\n";
        return false;
    }

    for (auto& request : requests_)
        camera_->queueRequest(request.get());

    open_ = true;
    lastFrameTime_ = std::chrono::high_resolution_clock::now();
    frameCount_ = 0;
    std::cout << "[RaspberryPiCameraSource] Opened '" << camera_->id()
              << "' at " << width_ << "x" << height_
              << " @ " << fps_ << " fps (Y stride " << yStride_ << " bytes)\n";
    return true;
}

void RaspberryPiCameraSource::onRequestComplete(Request* request)
{
    if (request->status() == Request::RequestCancelled)
        return;

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        completedRequests_.push(request);
    }
    queueCv_.notify_one();
}

bool RaspberryPiCameraSource::mapYPlane(const FrameBuffer* buffer, Frame& outFrame)
{
    const FrameBuffer::Plane& yPlane = buffer->planes()[0];

    // Map dmabuf for CPU read
    const size_t mapLength = yPlane.offset + yPlane.length;
    void* base = mmap(nullptr, mapLength, PROT_READ, MAP_SHARED, yPlane.fd.get(), 0);
    if (base == MAP_FAILED)
    {
        std::cerr << "[RaspberryPiCameraSource] mmap() failed\n";
        return false;
    }

    uint8_t* yData = static_cast<uint8_t*>(base) + yPlane.offset;

    // Wrap the Y plane as a CV_8UC1 Mat WITHOUT cloning.
    // The Frame now owns the mmap region and will munmap it when destroyed or moved.
    outFrame.image = cv::Mat(height_, width_, CV_8UC1, yData, yStride_);
    outFrame.mmapBase = base;
    outFrame.mmapLength = mapLength;

    return true;
}

bool RaspberryPiCameraSource::grabFrame(Frame& outFrame)
{
    if (!open_)
        return false;

    Request* request = nullptr;
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait_for(lock, std::chrono::seconds(2),
                           [this] { return !completedRequests_.empty(); });
        if (completedRequests_.empty())
            return false; // timed out -- camera stalled or unplugged
        request = completedRequests_.front();
        completedRequests_.pop();
    }

    FrameBuffer* buffer = request->buffers().at(stream_);
    if (!mapYPlane(buffer, outFrame))
        return false;

    outFrame.timestamp = std::chrono::steady_clock::now();
    static uint64_t seq = 0;
    outFrame.sequenceNumber = seq++;

    // Track frame arrival rate
    ++frameCount_;
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration<double>(now - lastFrameTime_).count();

    if (frameCount_ % 30 == 0)
    {
        double measuredFps = frameCount_ / elapsed;
        std::cout << "[RaspberryPiCameraSource] Camera frame arrival: " << measuredFps << " fps\n";
        lastFrameTime_ = now;
        frameCount_ = 0;
    }

    // Recycle the request so the camera can keep filling this buffer.
    request->reuse(Request::ReuseBuffers);
    camera_->queueRequest(request);

    return true;
}

void RaspberryPiCameraSource::close()
{
    if (!open_)
        return;

    camera_->stop();
    camera_->requestCompleted.disconnect(this);
    requests_.clear();
    allocator_.reset();
    camera_->release();
    camera_.reset();
    cameraManager_.reset();
    open_ = false;
}

bool RaspberryPiCameraSource::isOpen() const { return open_; }
int RaspberryPiCameraSource::width() const { return width_; }
int RaspberryPiCameraSource::height() const { return height_; }
double RaspberryPiCameraSource::fps() const { return fps_; }
std::string RaspberryPiCameraSource::name() const { return "RaspberryPiCameraSource (libcamera, 8-bit Y channel)"; }
