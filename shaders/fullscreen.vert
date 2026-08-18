#version 450

// Classic vertex-buffer-free fullscreen triangle: 3 vertices, positions and
// UVs derived from gl_VertexIndex. Covers the whole screen (the triangle
// extends past the clip volume on two corners, which is fine -- it gets
// clipped for free).
layout(location = 0) out vec2 vUv;

void main()
{
    vUv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUv * 2.0 - 1.0, 0.0, 1.0);
}
