#version 450
// The emulator picture: one nearest-neighbour sample of the 320x256 frame plus
// an optional CRT pass -- a dark seam between source lines and a faint aperture
// grille. Both are driven by uniforms so the strength can be turned down.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

// Set 2 holds samplers and set 3 uniform buffers, as SDL_GPU expects.
layout(set = 2, binding = 0) uniform sampler2D frameTex;

layout(set = 3, binding = 0) uniform UBO {
  vec4 dest;    // x, y, width, height of the picture, in pixels
  vec4 surface; // output width, output height, source width, source height
  vec4 params;  // scanline strength, mask strength, crt enabled, unused
} u;

void main() {
  // The triangle is larger than the picture; the excess is simply dropped.
  if (vUV.x < 0.0 || vUV.x > 1.0 || vUV.y < 0.0 || vUV.y > 1.0) discard;

  vec3 color = texture(frameTex, vUV).rgb;

  if (u.params.z > 0.5) {
    // One seam per source line, at the boundary between rows, so a Spectrum row
    // keeps a single edge no matter how far the window is zoomed.
    float row = fract(vUV.y * u.surface.w);
    float seam = smoothstep(0.5, 1.0, abs(row - 0.5) * 2.0);
    color *= 1.0 - u.params.x * seam;

    // Three phosphors per output pixel. Only the horizontal term is modelled:
    // at these sizes a vertical one would just shimmer.
    float grille = 0.5 + 0.5 * cos(gl_FragCoord.x * 2.0943951);
    color *= 1.0 - u.params.y * 0.25 * grille;
  }

  oColor = vec4(color, 1.0);
}
