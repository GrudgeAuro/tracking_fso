cmake_minimum_required(VERSION 3.16)
project(prbs_vision LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This build targets the Raspberry Pi 5 specifically: libcamera for the CSI
# cameras (HQ Camera / Camera Module 3), Vulkan 1.3 for the V3DV driver.

find_package(Vulkan 1.3 REQUIRED)
find_package(glfw3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# --- Shaders: compile GLSL -> SPIR-V at build time --------------------------
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SHADER_SOURCES
    ${SHADER_SRC_DIR}/fullscreen.vert
    ${SHADER_SRC_DIR}/display_16bit.frag
    ${SHADER_SRC_DIR}/bayer_to_mono.comp
    ${SHADER_SRC_DIR}/median_filter.comp
    ${SHADER_SRC_DIR}/local_minmax.comp
    ${SHADER_SRC_DIR}/local_contrast.comp
)
set(SHADER_SPIRV_OUTPUTS "")
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPIRV_OUT ${SHADER_OUT_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SPIRV_OUT}
        COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER} -o ${SPIRV_OUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME} -> SPIR-V"
    )
    list(APPEND SHADER_SPIRV_OUTPUTS ${SPIRV_OUT})
endforeach()
add_custom_target(shaders ALL DEPENDS ${SHADER_SPIRV_OUTPUTS})

# --- Main executable ---------------------------------------------------------
add_executable(prbs_vision
    src/main.cpp
    src/platform/RaspberryPiCameraSource.cpp
    src/gpu/VulkanContext.cpp
    src/gpu/VulkanDisplayStage.cpp
    src/gpu/VulkanRawProcessor.cpp
    src/gpu/VkUtils.cpp
)
add_dependencies(prbs_vision shaders)

target_include_directories(prbs_vision PRIVATE
    include
    src/platform
    src/gpu
    ${LIBCAMERA_INCLUDE_DIRS}
)

target_link_libraries(prbs_vision PRIVATE
    Vulkan::Vulkan
    glfw
    ${LIBCAMERA_LIBRARIES}
)
target_compile_options(prbs_vision PRIVATE ${LIBCAMERA_CFLAGS_OTHER})

# main.cpp loads shaders as "shaders/*.spv" relative to the working
# directory -- run the binary from the build directory, or copy the
# shaders/ folder alongside it.
