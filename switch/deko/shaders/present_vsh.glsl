// Full-screen triangle for the present and copy passes: three vertices from gl_VertexID, no vertex
// buffer. uv runs 0..1 across the screen (top left = 0,0), compiled by uam at build time.
#version 460
layout(location=0) out vec2 uv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  uv = p;
  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
