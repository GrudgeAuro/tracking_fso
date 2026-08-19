#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform usampler2D uYChannel; // R16_UINT, luma only

void main()
{
    uint raw = texture(uYChannel, vUv).r;
    // Normalize 16-bit unsigned to [0, 1] range for display
    float y = float(raw) / 65535.0;
    outColor = vec4(y, y, y, 1.0);
}
