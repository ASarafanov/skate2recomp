// Guest (Xenos) texture decoding for the native renderer.
//
// Adapted from Skate 3 Recomp's texture decode (EnsureGuestTextureFromWords,
// GetHostTextureFormat, SwapGuestEndian in skate3_native_scene_gpu*.{cpp,h},
// github.com/mchughalex/skate3recomp, by mchughalex, used with the author's
// permission). The layout math is the SDK's (Xenia's) TextureInfo and
// texture_util: mip locations, the 2D tiling and packed mips.

#include "native_texture.h"

#include <rex/graphics/native_rhi.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>
#include <vector>

namespace {

namespace nrhi = rex::graphics::nrhi;
namespace xenos = rex::graphics::xenos;

constexpr uint32_t kUploadPlacementAlignment = 512;

struct HostFormat {
  nrhi::Format format = nrhi::Format::kUnknown;
  uint32_t host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
};

bool GetHostFormat(xenos::TextureFormat format, HostFormat& out) {
  switch (rex::graphics::GetBaseFormat(format)) {
    case xenos::TextureFormat::k_DXT1:
      out = {nrhi::Format::kBC1_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_DXT2_3:
      out = {nrhi::Format::kBC2_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_DXT4_5:
      out = {nrhi::Format::kBC3_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_DXT5A:
      out = {nrhi::Format::kBC4_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case xenos::TextureFormat::k_DXN:
      out = {nrhi::Format::kBC5_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case xenos::TextureFormat::k_8_8_8_8:
      out = {nrhi::Format::kR8G8B8A8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_8:
      out = {nrhi::Format::kR8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case xenos::TextureFormat::k_8_8:
      out = {nrhi::Format::kR8G8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case xenos::TextureFormat::k_5_6_5:  // red/blue swapped while uploading
      out = {nrhi::Format::kB5G6R5_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB};
      return true;
    default:
      return false;
  }
}

// The fetch-constant swizzle composed over the host-format component remap
// (Xenos 0..5 = X, Y, Z, W, 0, 1, the same order as nrhi::Swizzle).
void ComposeSwizzle(uint32_t fetch_swizzle, uint32_t host_swizzle, nrhi::Swizzle out[4]) {
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t guest = (fetch_swizzle >> (3 * c)) & 7u;
    const uint32_t v = guest >= 4 ? guest : ((host_swizzle >> (3 * guest)) & 7u);
    out[c] = nrhi::Swizzle(v);
  }
}

void SwapEndian(uint8_t* data, uint32_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (uint32_t i = 0; i + 2 <= size; i += 2) std::swap(data[i], data[i + 1]);
      break;
    case xenos::Endian::k8in32:
      for (uint32_t i = 0; i + 4 <= size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (uint32_t i = 0; i + 4 <= size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

// Copy under SEH: texture data can be streamed out under us. Explicit
// volatile loads, not memcpy: a memcpy call inside __try can be emitted as a
// tail call outside the protected range (noted in Skate 3 Recomp).
bool TryCopy(uint8_t* dst, const uint8_t* src, size_t size) {
  __try {
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
      const uint64_t v = *reinterpret_cast<const volatile uint64_t*>(src + i);
      std::memcpy(dst + i, &v, 8);
    }
    for (; i < size; ++i) dst[i] = *reinterpret_cast<const volatile uint8_t*>(src + i);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

}  // namespace

bool DecodeNativeTexture(nrhi::Device* device, nrhi::Cmd* cmd, uint8_t* base,
                         const uint32_t words[6], NativeTexture& out) {
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dword_0 = words[0];
  fetch.dword_1 = words[1];
  fetch.dword_2 = words[2];
  fetch.dword_3 = words[3];
  fetch.dword_4 = words[4];
  fetch.dword_5 = words[5];
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.base_address == 0) return false;
  const rex::graphics::FormatInfo* pre = rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (!pre || pre->block_width == 0 || pre->block_height == 0 || pre->bytes_per_block() == 0) {
    return false;
  }
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) return false;
  HostFormat host;
  if (!GetHostFormat(info.format, host)) return false;
  if (info.dimension != xenos::DataDimension::k2DOrStacked || info.is_stacked ||
      info.width >= 8192 || info.height >= 8192 || info.memory.base_address == 0 ||
      info.memory.base_size == 0 || info.memory.base_size > 64u * 1024u * 1024u) {
    return false;
  }
  const rex::graphics::FormatInfo* fi = info.format_info();
  const uint32_t bpb = fi->bytes_per_block();
  if (bpb == 0 || (bpb & (bpb - 1)) != 0) return false;
  const uint32_t bpb_log2 = uint32_t(std::countr_zero(bpb));
  const uint32_t width = info.width + 1u, height = info.height + 1u;
  const uint32_t bw = fi->block_width, bh = fi->block_height;
  const uint32_t host_width = ((width + bw - 1) / bw) * bw;
  const uint32_t host_height = ((height + bh - 1) / bh) * bh;

  // Mip levels present in guest memory (power-of-two textures only, down to
  // 4x4 so block-compressed levels stay whole).
  const bool pow2 = (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
  uint32_t mip_count = 1;
  if (pow2 && info.memory.mip_address != 0) {
    const uint32_t avail = std::min(info.mip_levels(), info.GetMaxMipLevels());
    while (mip_count < avail && (width >> mip_count) >= 4 && (height >> mip_count) >= 4) {
      uint32_t ox = 0, oy = 0;
      if (info.GetMipLocation(mip_count, &ox, &oy, true) == 0) break;
      ++mip_count;
    }
  }

  struct Mip {
    uint32_t addr, size, pitch_blocks, ox, oy, cols, rows, upload_offset, upload_pitch;
  };
  std::vector<Mip> mips(mip_count);
  uint32_t upload_size = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    Mip& s = mips[m];
    uint32_t ox = 0, oy = 0;
    s.addr = info.GetMipLocation(m, &ox, &oy, true);
    const auto ext = info.GetMipExtent(m, true);
    s.pitch_blocks = m == 0 ? info.extent.block_pitch_h : ext.block_pitch_h;
    s.size = m == 0 ? info.memory.base_size : ext.all_blocks() * bpb;
    const uint32_t mw = std::max(width >> m, 1u), mh = std::max(height >> m, 1u);
    s.cols = (mw + bw - 1) / bw;
    s.rows = (mh + bh - 1) / bh;
    if (info.is_tiled) {
      s.size = std::max(s.size, rex::graphics::texture_util::GetTiledAddressUpperBound2D(
                                    s.cols + ox, s.rows + oy, s.pitch_blocks, bpb_log2));
    }
    s.ox = ox;
    s.oy = oy;
    s.upload_pitch = (s.cols * bpb + (nrhi::kRowPitchAlignment - 1u)) &
                     ~(nrhi::kRowPitchAlignment - 1u);
    s.upload_offset = (upload_size + (kUploadPlacementAlignment - 1u)) &
                      ~(kUploadPlacementAlignment - 1u);
    upload_size = s.upload_offset + s.upload_pitch * s.rows;
  }

  nrhi::TextureDesc desc;
  desc.kind = nrhi::TextureKind::k2D;
  desc.width = host_width;
  desc.height = host_height;
  desc.mip_levels = mip_count;
  desc.format = host.format;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  nrhi::Texture* texture = device->CreateTexture(desc);
  if (!texture) return false;
  nrhi::BufferDesc up;
  up.size = upload_size;
  up.heap = nrhi::HeapKind::kUpload;
  up.bind_class = nrhi::BufferBindClass::kCopySrc;
  nrhi::Buffer* upload = device->CreateBuffer(up);
  uint8_t* mapping = upload ? static_cast<uint8_t*>(device->Map(upload)) : nullptr;
  if (!mapping) {
    if (upload) device->DestroyDeferred(upload);
    device->DestroyDeferred(texture);
    return false;
  }

  const bool swap_rb_565 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_5_6_5;
  std::vector<uint8_t> guest, row;
  uint32_t mips_done = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    const Mip& s = mips[m];
    guest.resize(s.size);
    // The physical mirror at 0xA0000000 serves every texture address.
    if (s.addr == 0 || !TryCopy(guest.data(), base + (0xA0000000u | s.addr), s.size)) break;
    const uint32_t row_bytes = s.cols * bpb;
    row.resize(row_bytes);
    for (uint32_t by = 0; by < s.rows; ++by) {
      for (uint32_t bx = 0; bx < s.cols; ++bx) {
        uint32_t off;
        if (info.is_tiled) {
          off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
              int32_t(bx + s.ox), int32_t(by + s.oy), s.pitch_blocks, bpb_log2));
        } else {
          off = ((by + s.oy) * s.pitch_blocks + bx + s.ox) * bpb;
        }
        uint8_t* dst = row.data() + size_t(bx) * bpb;
        if (off + bpb <= s.size) {
          std::memcpy(dst, guest.data() + off, bpb);
        } else {
          std::memset(dst, 0, bpb);
        }
      }
      SwapEndian(row.data(), row_bytes, info.endianness);
      if (swap_rb_565) {
        for (uint32_t i = 0; i + 2 <= row_bytes; i += 2) {
          uint16_t v;
          std::memcpy(&v, row.data() + i, 2);
          v = uint16_t((v & 0x07E0u) | ((v >> 11) & 0x1Fu) | ((v & 0x1Fu) << 11));
          std::memcpy(row.data() + i, &v, 2);
        }
      }
      std::memcpy(mapping + s.upload_offset + size_t(by) * s.upload_pitch, row.data(), row_bytes);
    }
    ++mips_done;
  }
  device->Unmap(upload);
  if (mips_done == 0) {
    device->DestroyDeferred(upload);
    device->DestroyDeferred(texture);
    return false;
  }
  for (uint32_t m = 0; m < mips_done; ++m) {
    cmd->CopyBufferToTexture(texture, m, 0, upload, mips[m].upload_offset, mips[m].upload_pitch,
                             std::max(host_width >> m, 1u), std::max(host_height >> m, 1u), 1);
  }
  cmd->Barrier(texture, nrhi::ResourceState::kCopyDest, nrhi::ResourceState::kPixelShaderResource);
  device->DestroyDeferred(upload);

  nrhi::TextureViewDesc view;
  view.dimension = nrhi::ViewDimension::k2D;
  view.format = host.format;
  ComposeSwizzle(fetch.swizzle, host.host_swizzle, view.swizzle);
  view.mip_levels = mips_done;
  out.texture = texture;
  out.view = device->CreateTextureView(texture, view);
  out.width = width;
  out.height = height;
  return out.view != nullptr;
}

bool ReadNativeTextureFetch(uint8_t* base, uint32_t texture, uint32_t words[6]) {
  uint8_t raw[24];
  if (!texture || !TryCopy(raw, base + texture + 0x1C, sizeof(raw))) return false;
  for (int i = 0; i < 6; ++i) {
    words[i] = uint32_t(raw[i * 4]) << 24 | uint32_t(raw[i * 4 + 1]) << 16 |
               uint32_t(raw[i * 4 + 2]) << 8 | raw[i * 4 + 3];
  }
  return true;
}

bool DecodeNativeTextureObject(nrhi::Device* device, nrhi::Cmd* cmd, uint8_t* base,
                               uint32_t texture, NativeTexture& out) {
  uint32_t words[6];
  if (!ReadNativeTextureFetch(base, texture, words)) return false;
  return DecodeNativeTexture(device, cmd, base, words, out);
}

// Average colour of each face of a guest environment cube (DXT1 or 8888),
// faces in Xenos/D3D order +X, -X, +Y, -Y, +Z, -Z. The game samples its
// environment cubes at a deep gradient LOD, so the reflection is close to the
// face averages (Skate 3 Recomp's finding for the same engine).
bool NativeCubeFaceAverages(uint8_t* base, uint32_t texture, float out[6][4]) {
  uint32_t words[6];
  if (!ReadNativeTextureFetch(base, texture, words)) return false;
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dword_0 = words[0];
  fetch.dword_1 = words[1];
  fetch.dword_2 = words[2];
  fetch.dword_3 = words[3];
  fetch.dword_4 = words[4];
  fetch.dword_5 = words[5];
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.base_address == 0) return false;
  const rex::graphics::FormatInfo* pre = rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (!pre || pre->block_width == 0 || pre->block_height == 0 || pre->bytes_per_block() == 0) {
    return false;
  }
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) return false;
  const xenos::TextureFormat format = rex::graphics::GetBaseFormat(info.format);
  const bool dxt1 = format == xenos::TextureFormat::k_DXT1;
  if (info.dimension != xenos::DataDimension::kCube || info.memory.base_address == 0 ||
      (!dxt1 && format != xenos::TextureFormat::k_8_8_8_8) || info.width >= 1024) {
    return false;
  }
  const uint32_t bpb = dxt1 ? 8u : 4u;
  const uint32_t width = info.width + 1u, height = info.height + 1u;
  const uint32_t cols = dxt1 ? (width + 3) / 4 : width, rows = dxt1 ? (height + 3) / 4 : height;
  const uint32_t pitch = info.extent.block_pitch_h;
  const uint32_t slice = info.extent.block_pitch_h * info.extent.block_pitch_v * bpb;
  if (slice == 0 || slice > 4u * 1024u * 1024u) return false;
  std::vector<uint8_t> data(size_t(slice) * 6);
  if (!TryCopy(data.data(), base + (0xA0000000u | info.memory.base_address), data.size())) {
    return false;
  }
  // Bring a 16/32-bit guest word to host order.
  auto host16 = [&](const uint8_t* q) -> uint32_t {
    return fetch.endianness == xenos::Endian::k8in16 || fetch.endianness == xenos::Endian::k8in32
               ? uint32_t(q[0]) << 8 | q[1]
               : uint32_t(q[1]) << 8 | q[0];
  };
  const uint32_t bpb_log2 = dxt1 ? 3u : 2u;
  for (int face = 0; face < 6; ++face) {
    const uint8_t* f = data.data() + size_t(slice) * face;
    double sum[3] = {};
    double count = 0.0;
    for (uint32_t y = 0; y < rows; ++y) {
      for (uint32_t x = 0; x < cols; ++x) {
        const uint32_t offset =
            info.is_tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                                int32_t(x), int32_t(y), pitch, bpb_log2))
                          : (y * pitch + x) * bpb;
        if (offset + bpb > slice) continue;
        const uint8_t* b = f + offset;
        if (dxt1) {
          // Endian swaps move whole bytes, so the two colours sit in the first
          // two 16-bit words (k8in32 swaps them) and the 2-bit index fields
          // keep their counts.
          uint32_t c0 = host16(b), c1 = host16(b + 2);
          if (fetch.endianness == xenos::Endian::k8in32 || fetch.endianness == xenos::Endian::k16in32) {
            std::swap(c0, c1);
          }
          float pal[4][3];
          auto rgb = [](uint32_t c, float o[3]) {
            o[0] = float((c >> 11) & 31) / 31.0f;
            o[1] = float((c >> 5) & 63) / 63.0f;
            o[2] = float(c & 31) / 31.0f;
          };
          rgb(c0, pal[0]);
          rgb(c1, pal[1]);
          for (int k = 0; k < 3; ++k) {
            if (c0 > c1) {
              pal[2][k] = (2.0f * pal[0][k] + pal[1][k]) / 3.0f;
              pal[3][k] = (pal[0][k] + 2.0f * pal[1][k]) / 3.0f;
            } else {
              pal[2][k] = (pal[0][k] + pal[1][k]) * 0.5f;
              pal[3][k] = 0.0f;
            }
          }
          for (int i = 4; i < 8; ++i) {
            for (int bit = 0; bit < 8; bit += 2) {
              const int idx = (b[i] >> bit) & 3;
              for (int k = 0; k < 3; ++k) sum[k] += pal[idx][k];
              count += 1.0;
            }
          }
        } else {
          // 8888: ARGB in a big-endian word.
          const uint32_t w = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
          sum[0] += ((w >> 16) & 0xFF) / 255.0;
          sum[1] += ((w >> 8) & 0xFF) / 255.0;
          sum[2] += (w & 0xFF) / 255.0;
          count += 1.0;
        }
      }
    }
    if (count == 0.0) return false;
    for (int k = 0; k < 3; ++k) out[face][k] = float(sum[k] / count);
    out[face][3] = 1.0f;
  }
  return true;
}
