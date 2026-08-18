#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D uYChannel; // R8_UNORM, luma only

void main()
{
    float y = texture(uYChannel, vUv).r;
    outColor = vec4(y, y, y, 1.0);
}
