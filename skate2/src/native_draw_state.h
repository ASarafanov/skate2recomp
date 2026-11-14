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
};

// Per-frame constants of the game's main world shader.
struct NativeFrameConstants {
  bool valid = false;
  float fog[4] = {};        // VS c5: distance fog scale, bias, power
  float fog_color[4] = {};  // VS c6: linear fog colour
  float exposure = 2.0f;    // PS c6.x
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
