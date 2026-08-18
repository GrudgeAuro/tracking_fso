#include "RaspberryPiCameraSource.h"
#include <iostream>
#include <algorithm>
#include <sys/mman.h>

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

    // Try RAW first; fall back to VideoRecording if unavailable.
    config_ = camera_->generateConfiguration({ StreamRole::Raw });
    if (!config_ || config_->empty())
    {
        std::cerr << "[RaspberryPiCameraSource] RAW stream role not available; falling back to VideoRecording (Y plane)\n";
        config_ = camera_->generateConfiguration({ StreamRole::VideoRecording });
        if (!config_ || config_->empty())
        {
            std::cerr << "[RaspberryPiCameraSource] Failed to generate configuration\n";
            return false;
        }
    }

    StreamConfiguration& streamConfig = config_->at(0);
    streamConfig.size.width = width;
    streamConfig.size.height = height;

    // Prefer RAW SBGGR12; if not available, libcamera will adjust to an
    // available format (e.g., YUV420) during validate()/configure().
    streamConfig.pixelFormat = formats::SBGGR12;

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
    yStride_ = streamConfig.stride; // row stride of plane 0, in bytes
    stream_ = streamConfig.stream();

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

    // Map from offset 0 through (offset + length): planes can share a
    // single dmabuf fd at different offsets, so this is the only mapping
    // that's correct regardless of how the ISP packed Y/U/V for this format.
    const size_t mapLength = yPlane.offset + yPlane.length;
    void* base = mmap(nullptr, mapLength, PROT_READ, MAP_SHARED, yPlane.fd.get(), 0);
    if (base == MAP_FAILED)
    {
        std::cerr << "[RaspberryPiCameraSource] mmap() failed\n";
        return false;
    }

    uint8_t* yData = static_cast<uint8_t*>(base) + yPlane.offset;

    const size_t expected = static_cast<size_t>(yStride_) * static_cast<size_t>(height_);
    if (yPlane.length < expected)
    {
        std::cerr << "[RaspberryPiCameraSource] WARNING: Y plane length " << yPlane.length
                   << " < expected " << expected << " (stride " << yStride_
                   << " x height " << height_ << "). Verify against this libcamera version.\n";
    }

    // Detect whether the plane appears to be 8-bit (Y) or 16-bit RAW.
    const size_t bytesIf8 = static_cast<size_t>(yStride_) * static_cast<size_t>(height_);
    const size_t bytesIf16 = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2u;

    if (yPlane.length >= bytesIf16 && yStride_ >= static_cast<unsigned int>(width_ * 2))
    {
        // RAW 16-bit data path: interpret as CV_16UC1 raw Bayer (BGGR) and demosaic to 16-bit mono
        const size_t rowBytesNeeded = static_cast<size_t>(width_) * 2;
        cv::Mat raw16;
        if (static_cast<size_t>(yStride_) >= rowBytesNeeded) {
            raw16 = cv::Mat(height_, width_, CV_16UC1, yData, yStride_).clone();
        } else {
            raw16 = cv::Mat(height_, width_, CV_16UC1);
            for (int r = 0; r < height_; ++r)
            {
                memcpy(raw16.ptr(r), yData + (size_t)r * yStride_, rowBytesNeeded);
            }
        }

        // Convert raw16 (16-bit container with left-aligned 12-bit samples) to 12-bit samples
        // by shifting right 4 bits, then perform a simple demosaic to monochrome.
        cv::Mat raw12(height_, width_, CV_16UC1);
        for (int r = 0; r < height_; ++r)
        {
            uint16_t* s = raw16.ptr<uint16_t>(r);
            uint16_t* d = raw12.ptr<uint16_t>(r);
            for (int c = 0; c < width_; ++c)
                d[c] = static_cast<uint16_t>(s[c] >> 4);
        }

        cv::Mat mono16(height_, width_, CV_16UC1);
        auto get = [&](int rr, int cc) -> uint16_t {
            int rclamped = std::min(std::max(rr, 0), height_ - 1);
            int cclamped = std::min(std::max(cc, 0), width_ - 1);
            return raw12.ptr<uint16_t>(rclamped)[cclamped];
        };

        for (int r = 0; r < height_; ++r)
        {
            uint16_t* dst = mono16.ptr<uint16_t>(r);
            for (int c = 0; c < width_; ++c)
            {
                bool rowEven = (r % 2) == 0;
                bool colEven = (c % 2) == 0;
                float R, G, B;
                if (rowEven && colEven) {
                    B = get(r, c);
                    G = (get(r, c-1) + get(r, c+1) + get(r-1, c) + get(r+1, c)) / 4.0f;
                    R = (get(r-1, c-1) + get(r-1, c+1) + get(r+1, c-1) + get(r+1, c+1)) / 4.0f;
                } else if (rowEven && !colEven) {
                    G = get(r, c);
                    B = (get(r, c-1) + get(r, c+1)) / 2.0f;
                    R = (get(r-1, c) + get(r+1, c)) / 2.0f;
                } else if (!rowEven && colEven) {
                    G = get(r, c);
                    B = (get(r-1, c) + get(r+1, c)) / 2.0f;
                    R = (get(r, c-1) + get(r, c+1)) / 2.0f;
                } else {
                    R = get(r, c);
                    G = (get(r, c-1) + get(r, c+1) + get(r-1, c) + get(r+1, c)) / 4.0f;
                    B = (get(r-1, c-1) + get(r-1, c+1) + get(r+1, c-1) + get(r+1, c+1)) / 4.0f;
                }

                float y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                if (y < 0.0f) y = 0.0f;
                if (y > 4095.0f) y = 4095.0f;
                uint16_t y16 = static_cast<uint16_t>(std::round(y)) << 4;
                dst[c] = y16;
            }
        }

        outFrame.image = mono16;
        munmap(base, mapLength);
        std::cerr << "[mapYPlane] used RAW demosaic -> CV_16UC1 mono\n";
        return true;
    }

    // Fallback: 8-bit Y plane path — wrap, clone, promote to 16-bit full range
    cv::Mat wrapped8(height_, width_, CV_8UC1, yData, yStride_);
    cv::Mat contig8 = wrapped8.clone();
    cv::Mat out16;
    contig8.convertTo(out16, CV_16U, 257.0); // map 0..255 -> 0..65535
    outFrame.image = out16;

    munmap(base, mapLength);
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
std::string RaspberryPiCameraSource::name() const { return "RaspberryPiCameraSource (libcamera, RAW)"; }
