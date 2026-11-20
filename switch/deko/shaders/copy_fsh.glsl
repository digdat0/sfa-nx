// 1:1 copy by draw (draw_copy in backend_commands.cpp): reads the exact source texel with texelFetch, so
// no filtering changes the image. Used for copies out of hardware-compressed render targets.
#version 460
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(binding=0) uniform sampler2D image;
void main() { color = texelFetch(image, ivec2(uv * vec2(textureSize(image, 0))), 0); }
