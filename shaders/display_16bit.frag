#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform usampler2D uImage;

void main()
{
    uint value = texture(uImage, vUv).r;
    float y = float(value) / 65535.0;
    outColor = vec4(y, y, y, 1.0);
}
