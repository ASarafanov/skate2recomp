// Native world renderer, first stage (see native_world.cpp).

#pragma once

#include <cstdint>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

// Capture, called from the guest hooks in native_probe.cpp.
void NativeWorldOnDrawList(uint8_t* base, uint32_t view, uint32_t first, uint32_t count);
// RenderMesh (entities: skater, pedestrians, props): r4 = MeshContext.
void NativeWorldOnRenderMesh(uint8_t* base, uint32_t mesh_context);
void NativeWorldOnSwap();

// renderengine::Texture registered under an asset GUID (0 = unknown).
uint32_t NativeWorldTextureForGuid(uint64_t guid);

// Called on every presented frame, menus included: prepares GPU state
// (pipeline variants) before the world is first drawn.
void NativeWorldPrepare(const rex::graphics::NativeGuestOutputRenderContext& context);

// Draws the last captured frame into the guest output. Returns false (the
// emulated frame is shown) when disabled or when nothing was captured.
// `force` enables it regardless of the skate2_native_world cvar (flag file).
bool NativeWorldRender(const rex::graphics::NativeGuestOutputRenderContext& context,
                       bool force = false);
