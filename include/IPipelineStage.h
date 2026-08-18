#pragma once

#include "Frame.h"

// Every consumer of the camera stream implements this: the Vulkan display
// stage today, and later e.g. the PRBS correlator/tracker, and/or an
// Ethernet sink streaming frames to an external FPGA. main.cpp just chains
// stages together against this interface.
class IPipelineStage
{
public:
    virtual ~IPipelineStage() = default;

    virtual bool init(int width, int height) = 0;

    // Consume a frame. Returns false to signal the pipeline should stop
    // (e.g. the display window was closed).
    virtual bool process(const Frame& frame) = 0;

    virtual void shutdown() = 0;

    virtual const char* name() const = 0;
};
