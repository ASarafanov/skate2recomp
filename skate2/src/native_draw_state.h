// Per-draw render state of the game's emulated draws, captured from the
// command processor (see native_draw_state.cpp).

#pragma once

#include <cstdint>

struct NativeDrawState {
  // Alpha test: pass when alpha >= alpha_ref (0 = off). The game's world
  // shaders output the diffuse alpha.
  float alpha_ref = 0.0f;
  enum Blend : uint8_t { kOpaque = 0, kAlpha, kAdditive } blend = kOpaque;
  // Culling in the native RHI's convention (D3D: clockwise front faces).
  enum Cull : uint8_t { kNone = 0, kFront, kBack } cull = kBack;
  bool depth_write = true;
  bool depth_offset = false;  // polygon offset (decals)
  uint64_t pixel_shader = 0;
  // Main world material (vertex shader E5C957221BEF0ECD): VS c7.x macro
  // overlay UV scale, c8.x detail UV scale.
  float macro_scale = 0.0f;
  float detail_scale = 0.0f;
};

// Per-frame constants of the game's main world shader.
struct NativeFrameConstants {
  bool valid = false;
  float fog[4] = {};        // VS c5: distance fog scale, bias, power
  float fog_color[4] = {};  // VS c6: linear fog colour
  float exposure = 2.0f;    // PS c6.x
  bool sun_valid = false;
  float sun[4] = {};        // main world material PS c4: direction to the sun
  // Main world material PS c0..c3: the sun shadow projection. Light-space
  // X = c0.p, Y = c2.p (first cascade), the second cascade c1.xy * (X, Y) +
  // c1.zw, depth c3.p.
  bool shadow_valid = false;
  float shadow[16] = {};
};

// Registers the observer with the SDK (OnPreSetup).
void InstallSkate2DrawObserver();

// Called once per presented frame on the command processor thread, before
// the native renderer runs.
void NativeDrawStateNewFrame();

// State of the emulated draw with this vertex buffer and first index (guest
// physical addresses) seen in the last two frames. False when unknown.
bool NativeDrawStateLookup(uint32_t vertex_buffer, uint32_t index_base, NativeDrawState& out);

const NativeFrameConstants& NativeDrawFrameConstants();

// Shader constants (256 float4 per stage, the stage's view) of an emulated
// draw. The renderer asks for the draws it needs (entities: their clip
// matrix, bone palette and lighting live there); the observer keeps a copy of
// those from then on.
struct NativeDrawConstants {
  uint64_t vertex_shader = 0;
  uint64_t pixel_shader = 0;
  const float* vs = nullptr;
  const float* ps = nullptr;
};
void NativeDrawStateWantConstants(uint32_t vertex_buffer, uint32_t index_base);
// Constants of that draw seen in the last two frames; false when not seen.
bool NativeDrawStateConstants(uint32_t vertex_buffer, uint32_t index_base,
                              NativeDrawConstants& out);
