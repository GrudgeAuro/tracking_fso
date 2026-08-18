#include "RaspberryPiCameraSource.h"

#include <algorithm>
#include <chrono>
#include <iostream>

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
        std::cerr << "[RaspberryPiCameraSource] No cameras detected\n";
        return false;
    }

    if (!cameraIdSubstring_.empty())
    {
        auto it = std::find_if(cameras.begin(), cameras.end(),
            [this](const auto& cam)
            {
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
        std::cerr << "[RaspberryPiCameraSource] Failed to acquire camera '"
                  << camera_->id() << "'\n";
        return false;
    }

    config_ = camera_->generateConfiguration({ StreamRole::Raw });
    if (!config_ || config_->empty())
    {
        std::cerr << "[RaspberryPiCameraSource] Failed to generate RAW configuration\n";
        return false;
    }

    StreamConfiguration& streamConfig = config_->at(0);
    streamConfig.size.width = width;
    streamConfig.size.height = height;
    streamConfig.pixelFormat = formats::SBGGR12;
    streamConfig.colorSpace = ColorSpace::Raw;

    const CameraConfiguration::Status status = config_->validate();
    if (status == CameraConfiguration::Invalid)
    {
        std::cerr << "[RaspberryPiCameraSource] Invalid RAW configuration\n";
        return false;
    }

    if (status == CameraConfiguration::Adjusted)
        std::cerr << "[RaspberryPiCameraSource] RAW configuration was adjusted by libcamera\n";

    if (camera_->configure(config_.get()))
    {
        std::cerr << "[RaspberryPiCameraSource] configure() failed\n";
        return false;
    }

    width_ = static_cast<int>(streamConfig.size.width);
    height_ = static_cast<int>(streamConfig.size.height);
    rawStride_ = streamConfig.stride;
    rawPixelFormat_ = streamConfig.pixelFormat;
    stream_ = streamConfig.stream();

    allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
    if (allocator_->allocate(stream_) < 0)
    {
        std::cerr << "[RaspberryPiCameraSource] RAW buffer allocation failed\n";
        return false;
    }

    const auto& buffers = allocator_->buffers(stream_);
    if (buffers.empty())
    {
        std::cerr << "[RaspberryPiCameraSource] No RAW buffers allocated\n";
        return false;
    }

    rawBufferSize_ = 0;
    for (const auto& buffer : buffers)
    {
        for (const auto& plane : buffer->planes())
        {
            rawBufferSize_ = std::max(
                rawBufferSize_,
                static_cast<size_t>(plane.offset) +
                static_cast<size_t>(plane.length));
        }

        auto request = camera_->createRequest();
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

    camera_->requestCompleted.connect(
        this, &RaspberryPiCameraSource::onRequestComplete);

    ControlList controls;
    int64_t frameDurationUs = 0;

    if (fps > 0)
    {
        frameDurationUs = 1'000'000 / fps;
    }
    else
    {
        auto it = camera_->controls().find(&controls::FrameDurationLimits);
        if (it != camera_->controls().end())
            frameDurationUs = it->second.min().get<int64_t>();
        else
            frameDurationUs = 1'000'000 / 30;
    }

    controls.set(
        controls::FrameDurationLimits,
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

    std::cout << "[RaspberryPiCameraSource] RAW camera opened at "
              << width_ << "x" << height_ << " @ " << fps_ << " fps\n"
              << "[RaspberryPiCameraSource] RAW stride: " << rawStride_ << " bytes\n"
              << "[RaspberryPiCameraSource] RAW pixel format: "
              << rawPixelFormat_.toString() << "\n"
              << "[RaspberryPiCameraSource] GPU path: DMA-BUF -> Vulkan (no CPU pixel copy)\n";

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

bool RaspberryPiCameraSource::mapRawPlane(
    const FrameBuffer* buffer,
    Request* request,
    Frame& outFrame)
{
    if (!buffer || !request || buffer->planes().empty())
        return false;

    const FrameBuffer::Plane& plane = buffer->planes()[0];
    if (!plane.fd.isValid())
        return false;

    const size_t expectedBytes =
        static_cast<size_t>(rawStride_) * static_cast<size_t>(height_);

    if (plane.length < expectedBytes)
    {
        std::cerr << "[RaspberryPiCameraSource] RAW plane is smaller than expected: "
                  << plane.length << " < " << expectedBytes << "\n";
        return false;
    }

    outFrame.dmabufFd = plane.fd.get();
    outFrame.bufferSize = static_cast<uint32_t>(rawBufferSize_);
    outFrame.planeOffset = plane.offset;
    outFrame.planeLength = plane.length;
    outFrame.strideBytes = rawStride_;
    outFrame.widthPixels = static_cast<uint32_t>(width_);
    outFrame.heightPixels = static_cast<uint32_t>(height_);
    outFrame.timestamp = std::chrono::steady_clock::now();

    static uint64_t sequence = 0;
    outFrame.sequenceNumber = sequence++;

    outFrame.releaseCallback = std::make_shared<std::function<void()>>([this, request]()
    {
        recycleRequest(request);
    });

    return true;
}

void RaspberryPiCameraSource::recycleRequest(Request* request)
{
    if (!request || !camera_ || !open_)
        return;

    request->reuse(Request::ReuseBuffers);
    camera_->queueRequest(request);
}

bool RaspberryPiCameraSource::grabFrame(Frame& outFrame)
{
    if (!open_)
        return false;

    outFrame = {};

    Request* request = nullptr;
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait_for(lock, std::chrono::seconds(2),
            [this] { return !completedRequests_.empty(); });

        if (completedRequests_.empty())
            return false;

        request = completedRequests_.front();
        completedRequests_.pop();
    }

    FrameBuffer* buffer = request->buffers().at(stream_);

    if (!mapRawPlane(buffer, request, outFrame))
    {
        recycleRequest(request);
        return false;
    }

    return true;
}

void RaspberryPiCameraSource::close()
{
    if (!cameraManager_)
        return;

    if (camera_)
    {
        camera_->stop();
        camera_->requestCompleted.disconnect(this);
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        std::queue<Request*> empty;
        completedRequests_.swap(empty);
    }

    requests_.clear();
    allocator_.reset();

    if (camera_)
    {
        camera_->release();
        camera_.reset();
    }

    cameraManager_.reset();

    open_ = false;
    width_ = 0;
    height_ = 0;
    rawStride_ = 0;
    rawBufferSize_ = 0;
    fps_ = 0.0;
}

bool RaspberryPiCameraSource::isOpen() const { return open_; }
int RaspberryPiCameraSource::width() const { return width_; }
int RaspberryPiCameraSource::height() const { return height_; }
double RaspberryPiCameraSource::fps() const { return fps_; }
std::string RaspberryPiCameraSource::name() const
{
    return "RaspberryPiCameraSource (libcamera, RAW Bayer DMA-BUF)";
}
