// Debug builds only (gx_probe.inc): paints a draw's pixels magenta to show where it lands.
#version 460
layout(location=0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 1.0, 1.0); }
