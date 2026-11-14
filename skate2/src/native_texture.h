// Guest (Xenos) texture decoding for the native renderer (see native_texture.cpp).

#pragma once

#include <cstdint>

namespace rex::graphics::nrhi {
class Cmd;
class Device;
class Texture;
class TextureView;
}  // namespace rex::graphics::nrhi

struct NativeTexture {
  rex::graphics::nrhi::Texture* texture = nullptr;
  rex::graphics::nrhi::TextureView* view = nullptr;
  uint32_t width = 0, height = 0;
};

// Decodes the texture whose six fetch-constant words (host order) are given:
// untiles every mip level, swaps it to host endianness and records the upload
// copies on `cmd` (outside a render pass). Returns false for unsupported
// formats or unreadable data.
bool DecodeNativeTexture(rex::graphics::nrhi::Device* device, rex::graphics::nrhi::Cmd* cmd,
                         uint8_t* base, const uint32_t words[6], NativeTexture& out);

// Reads the fetch constant of a renderengine::Texture (a D3D texture header:
// the six words start at +0x1C) and decodes it.
bool DecodeNativeTextureObject(rex::graphics::nrhi::Device* device,
                               rex::graphics::nrhi::Cmd* cmd, uint8_t* base, uint32_t texture,
                               NativeTexture& out);
