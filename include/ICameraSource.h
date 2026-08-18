#pragma once

#include "Frame.h"
#include <string>

// ICameraSource is the common image source class: everything downstream
// (Vulkan display today, an Ethernet/FPGA sink later) only ever depends on
// this interface and on Frame -- never on libcamera types directly. That's
// what lets the same captured stream feed a second consumer later without
// touching the capture code at all.
//
// Currently there's a single implementation, RaspberryPiCameraSource
// (libcamera, CSI cameras only -- this project targets the Pi 5
// specifically). The interface is still kept separate from that concrete
// class so a synthetic/test-pattern source or a recorded-file source can be
// dropped in later without disturbing anything else.
class ICameraSource
{
public:
    virtual ~ICameraSource() = default;

    // width/height: requested resolution. libcamera will snap this to the
    // nearest supported sensor mode.
    // fps: requested frame rate. Pass 0 to request the fastest frame rate
    // the selected sensor mode supports ("max FPS").
    virtual bool open(int width, int height, int fps) = 0;

    // Blocking grab of the next frame. Returns false if a frame could not
    // be retrieved (device error, stream stalled, etc).
    virtual bool grabFrame(Frame& outFrame) = 0;

    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;

    // Actual configured frame rate (may differ slightly from requested fps
    // due to sensor mode quantization -- useful for logging/telemetry).
    virtual double fps() const = 0;

    virtual std::string name() const = 0;
};
