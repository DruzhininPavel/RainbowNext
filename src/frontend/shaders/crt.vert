#version 450
// A single triangle that covers the destination rectangle. SDL_GPU hands the
// vertex index to us, so there is no vertex buffer to upload every frame.

layout(location = 0) out vec2 vUV;

// Set 1 is where SDL_GPU wants vertex-stage uniform buffers.
layout(set = 1, binding = 0) uniform UBO {
  vec4 dest;    // x, y, width, height of the picture, in pixels
  vec4 surface; // output width, output height, source width, source height
  vec4 params;  // scanline strength, mask strength, crt enabled, unused
} u;

void main() {
  vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  vUV = corner;

  // SDL_GPU's clip space has +Y up while the destination rectangle is given in
  // viewport pixels with +Y down, so the vertical axis is flipped here.
  vec2 pixel = u.dest.xy + corner * u.dest.zw;
  vec2 ndc = pixel / u.surface.xy * 2.0 - 1.0;
  gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
