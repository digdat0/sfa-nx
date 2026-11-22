// Present: samples the finished frame with the bound (linear) sampler, scaling it to the swapchain size.
// SFA-NX: alpha is written as 1 (Aurora's fs_opaque did the same): with the direct present the source is
// the EFB itself, whose alpha channel holds the game's destination alpha, not coverage.
#version 460
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(binding=0) uniform sampler2D image;
void main() { color = vec4(texture(image, uv).rgb, 1.0); }
