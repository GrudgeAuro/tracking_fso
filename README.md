# prbs-vision

Modular, GPU-accelerated signal-processing pipeline for the PRBS
image-acquisition/tracking project. **Targets the Raspberry Pi 5
specifically** — Vulkan 1.3 via the V3DV driver, libcamera for the CSI
cameras.

## Why Vulkan, why RPi5-only

Desktop OpenGL on the Pi 5's V3D (VideoCore VII) driver is capped at
**3.1 and explicitly non-conformant**. Vulkan on the same hardware is
**1.3-conformant** (since Mesa 24.3, Aug 2024). Vulkan is also the one
GPU API here with real compute-shader support for the PRBS correlator
stage that's coming next, so this project standardizes on it instead of
carrying a second, weaker GL path.

Because the cameras (Raspberry Pi HQ Camera / IMX477, Camera Module 3 NoIR /
IMX708) are CSI-only, there's no meaningful "capture" story on a plain
Ubuntu desktop anyway — so rather than keep a `RUN_PLATFORM` switch for a
source that only exists on one platform, the camera code now targets the Pi
5 directly. `ICameraSource` stays as a plain interface so a synthetic/file
source can be added later for pure-software testing on Ubuntu without
touching anything else.

## Stage 0: camera feed (Y channel only) -> screen

```
ICameraSource (interface)
 └── RaspberryPiCameraSource     libcamera, CSI camera, YUV420 -> Y plane only
              │
              ▼  Frame { cv::Mat CV_8UC1 (Y only), timestamp, seq }
              │
      VulkanDisplayStage (implements IPipelineStage)
      uploads Frame -> R8_UNORM texture -> dynamic-rendering draw
```

**Why Y only:** the PRBS tracking work is luma-based, and dropping chroma
halves the data volume before it ever needs to move anywhere — including
over Ethernet to an external FPGA, which is the other consumer this is
built for (see below). Capturing in YUV420 costs nothing extra either: it's
the ISP's native output format, so grabbing plane 0 is just "don't bother
reading planes 1/2," not a conversion pass.

**Why a "common image source class":** `ICameraSource` + `Frame` don't know
anything about Vulkan or displays. `Frame` is just a timestamped, tightly
packed byte buffer. That's deliberate — when you're ready to also stream
frames to an FPGA over Ethernet, that sink is a new class implementing
`IPipelineStage` (packetize `frame.image.data`, `frame.width()`,
`frame.height()` over UDP/TCP) sitting *next to* `VulkanDisplayStage`, not
replacing it. `main.cpp` can feed both from the same `grabFrame()` call.
`RaspberryPiCameraSource` doesn't change either way.

## Project layout

```
include/                 Platform-agnostic interfaces & shared types
  ICameraSource.h          camera source contract (width/height/fps primitives)
  IPipelineStage.h         contract every consumer implements
  Frame.h                  Y-only frame data that flows stage-to-stage

src/platform/
  RaspberryPiCameraSource.*  libcamera CSI capture, Y-plane extraction, max-FPS logic

src/gpu/
  VulkanContext.*          instance/device/swapchain/sync -- shared by every GPU stage
  VkUtils.*                buffer/image/shader-module helpers, reused across stages
  VulkanDisplayStage.*     Stage 0: Y-channel texture upload + dynamic-rendering draw

shaders/
  fullscreen.vert           vertex-buffer-free fullscreen triangle
  y_channel.frag            samples the R8 Y texture, outputs grayscale

src/main.cpp              Resolution/FPS primitives + the capture/display loop
CMakeLists.txt
```

## Building on the Raspberry Pi 5

```bash
sudo apt install cmake build-essential pkg-config \
                  libcamera-dev libcamera-tools \
                  libvulkan-dev vulkan-tools glslang-tools \
                  mesa-vulkan-drivers \
                  libglfw3-dev libopencv-dev

mkdir build && cd build
cmake ..
make -j$(nproc)
./prbs_vision
```

Run it **from the `build/` directory** — the shaders are compiled to
`build/shaders/*.spv` and loaded by relative path at startup. Esc or
closing the window exits.

Set `kEnableVulkanValidation = false` in `main.cpp` once things are stable;
the validation layer (from `vulkan-validationlayers`, install if you want
it) adds overhead you don't need once the pipeline's proven out.

## Camera configuration (`src/main.cpp`)

```cpp
constexpr int kWidth = 1332;
constexpr int kHeight = 990;
constexpr int kFpsRequest = 0;              // 0 = max FPS for the selected mode
constexpr const char* kCameraIdSubstring = ""; // "" = first camera libcamera reports
```

- **Resolution** gets snapped to the nearest sensor mode by libcamera; the
  actual configured size is logged at startup.
- **FPS = 0 means max**: the code queries `FrameDurationLimits` for the
  mode libcamera selected and requests its fastest frame duration, rather
  than guessing a number. The achieved FPS is logged at startup and
  available via `camera.fps()`.
- If you have both the HQ Camera and Camera Module 3 attached at once (Pi 5
  has two CSI ports), run once to see the detected camera IDs printed to
  the console, then set `kCameraIdSubstring` to pick one.
- Actual max FPS is sensor-mode dependent. Run `libcamera-hello
  --list-cameras` on the Pi to see the exact modes/frame-duration ranges
  your specific sensor + libcamera version reports, and set
  `kWidth`/`kHeight` to the mode you want the max FPS of.

## What I verified vs. what needs the real hardware

Built and compiled in a sandboxed Ubuntu environment against the real
Vulkan 1.3 SDK, libcamera 0.2.0 headers, and glslang:

- **Every source file compiles clean** against real Vulkan/libcamera/OpenCV
  headers (no stub/mock headers).
- **Both shaders compile to SPIR-V and pass `spirv-val`.**
- **The full project configures, builds, and links** with zero errors/warnings.
- **The binary starts, initializes libcamera, and fails gracefully** (clear
  error message, clean exit) when no CSI camera is present — proving the
  error path doesn't crash, though the happy path (an actual camera image
  making it to the screen) can only be confirmed on the Pi 5 itself, which
  wasn't available in this environment.

Two things worth a first-run sanity check on your actual hardware:
1. `StreamConfiguration::stride` is used directly as the Y-plane row
   stride — this is standard for libcamera's planar YUV420 output, but
   worth confirming against the stride your Pi 5's specific libcamera
   build reports (a mismatch would show up as a sheared/skewed image, and
   `mapYPlane()` logs a warning if the reported plane length looks off).
2. `FrameDurationLimits` querying for "max FPS" — the logged fps at
   startup should match what you'd expect from the sensor's datasheet for
   the mode chosen; if it doesn't, `libcamera-hello --list-cameras` will
   show you what libcamera actually thinks the mode's range is.

## Adding the next stage (e.g. PRBS correlator)

1. New file(s) under `src/gpu/`, implementing `IPipelineStage`, using
   `VulkanContext`'s existing device/queue/command pool (don't create a
   second `VulkanContext` — pass a reference in, or promote ownership of
   `VulkanContext` from `VulkanDisplayStage` up into `main.cpp` once there's
   more than one GPU stage sharing it).
2. Compute shaders go in `shaders/`, added to `SHADER_SOURCES` in
   `CMakeLists.txt`.
3. Insert the stage into the loop in `main.cpp`.

Nothing in `ICameraSource`, `Frame`, or `RaspberryPiCameraSource` needs to
change — that's the seam this was built around.
