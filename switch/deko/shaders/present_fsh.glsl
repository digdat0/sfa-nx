// Present: samples the finished frame with the bound (linear) sampler, scaling it to the swapchain size.
#version 460
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(binding=0) uniform sampler2D image;
void main() { color = texture(image, uv); }
