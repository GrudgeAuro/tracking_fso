# PRBS Vision - Raspberry Pi 5 GPU RAW pipeline

This version uses the IMX477 RAW Bayer DMA-BUF directly from libcamera and performs the image-processing path on the Raspberry Pi 5 V3D GPU.

## Pipeline

```text
IMX477 SBGGR12
     |
     | libcamera SBGGR16 DMA-BUF
     v
Vulkan external-memory import
     |
     +--> Bayer -> 16-bit monochrome
     |
     +--> 3x3 median filter
     |
     +--> 128x128 local min/max contrast stretch
     |
     v
16-bit monochrome GPU image
     |
     v
Vulkan display
```

No CPU pixel buffer, `cv::Mat`, `memcpy()`, `mmap()` or CPU Bayer conversion is used in the capture/processing path.

The IMX477 stream is requested as `SBGGR12`. On the Pi 5 configuration used during development, libcamera exposes the buffer as `SBGGR16`, with the 12 valid bits left aligned. The GPU therefore reads the 16-bit values directly; no CPU-side shift is performed.

## GPU stages

### 1. Bayer -> monochrome

`bayer_to_mono.comp` averages a 2x2 Bayer neighbourhood while retaining full output resolution. The arithmetic operates directly on the left-aligned 16-bit RAW samples.

### 2. Median

`median_filter.comp` performs a 3x3 median filter on the 16-bit monochrome image.

### 3. Local contrast

`local_minmax.comp` uses atomic min/max operations into GPU SSBOs for 128x128 tiles. The buffers are initialized with `vkCmdFillBuffer`, so initialization is also GPU-side.

`local_contrast.comp` maps each pixel between the local tile minimum and maximum into the complete 16-bit output range.

This is local min/max contrast stretching rather than full CLAHE/histogram equalization.

## Build

The project expects `glslangValidator` to be installed. CMake compiles all GLSL shaders into SPIR-V.

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

Run from the build directory so the executable can find the generated `shaders/*.spv` files:

```bash
./prbs_vision
```

## Important

The camera DMA-BUF is not recycled until the Vulkan frame fence for the submission that consumed it has completed. This prevents libcamera from overwriting a buffer while the GPU is still reading it.
