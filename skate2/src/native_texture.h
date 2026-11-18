// Guest (Xenos) texture decoding for the native renderer (see native_texture.cpp).

#pragma once

#include <cstdint>
#include <vector>

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
// The fetch constant of a guest renderengine::Texture (six words at +0x1C,
// host order). The game rewrites it when a texture's full mips stream in.
bool ReadNativeTextureFetch(uint8_t* base, uint32_t texture, uint32_t words[6]);

// Average colour of each face of a guest environment cube (+X, -X, +Y, -Y,
// +Z, -Z); false when the texture is not a DXT1/8888 cube.
bool NativeCubeFaceAverages(uint8_t* base, uint32_t texture, float out[6][4]);

// A guest 3D colour table (k_8_8_8_8, n x n x n, n <= 64) given by its fetch
// constant, unrolled into an RGBA8 atlas of n slices side by side (n*n x n,
// texel (x + z*n, y)). False for other formats or unreadable data.
bool NativeLutAtlas(uint8_t* base, const uint32_t words[6], std::vector<uint8_t>& rgba,
                    uint32_t& n);

bool DecodeNativeTextureObject(rex::graphics::nrhi::Device* device,
                               rex::graphics::nrhi::Cmd* cmd, uint8_t* base, uint32_t texture,
                               NativeTexture& out);
