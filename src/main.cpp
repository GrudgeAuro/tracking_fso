#include "RaspberryPiCameraSource.h"
#include "VulkanDisplayStage.h"
#include "Frame.h"

#include <iostream>

namespace
{
// --- Resolution / FPS primitives -------------------------------------------
// fps = 0 means "max": RaspberryPiCameraSource queries the sensor mode
// selected for this resolution and requests its fastest frame duration.
//
// Reference max-FPS modes (check `libcamera-hello --list-cameras` on your
// actual Pi 5 -- exact modes/binning can shift between libcamera releases):
//   HQ Camera (IMX477):        1332x990 @ ~40fps binned,  2028x1080 @ ~50fps
//   Camera Module 3 (IMX708):  1536x864 @ ~120fps binned, 2304x1296 @ ~56fps
constexpr int kWidth = 1332;
constexpr int kHeight = 990;
constexpr int kFpsRequest = 120; // 0 = max FPS for the selected mode

// Leave empty to use whichever camera libcamera reports first. If both an
// HQ Camera and a Camera Module 3 are attached at once, pass a substring of
// the libcamera camera ID here (printed at startup) to pick one explicitly.
constexpr const char* kCameraIdSubstring = "0";

constexpr bool kEnableVulkanValidation = true; // turn off once things are stable
} // namespace

int main()
{
    RaspberryPiCameraSource camera(kCameraIdSubstring);
    if (!camera.open(kWidth, kHeight, kFpsRequest))
    {
        std::cerr << "Failed to open camera source: " << camera.name() << "\n";
        return 1;
    }

    VulkanDisplayStage display("PRBS Vision - Y Channel", kEnableVulkanValidation);
    if (!display.init(camera.width(), camera.height()))
    {
        std::cerr << "Failed to initialize display stage\n";
        return 1;
    }

    std::cout << "Running " << camera.name()
              << " -> " << display.name()
              << " @ " << camera.fps() << " fps"
              << "  (Esc or close window to quit)\n";

    Frame frame;
    while (camera.grabFrame(frame))
    {
        if (!display.process(frame))
            break;
    }

    display.shutdown();
    camera.close();
    return 0;
}
