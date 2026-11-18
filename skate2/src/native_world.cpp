// Native world renderer, first stage: untextured static world geometry.
//
// Capture (guest render thread): the scene draw list (sub_82364648) hands us
// every MeshContext of the frame. For each we keep the mesh object and its
// draw records; the D3D swap publishes the frame together with the camera
// view-projection (view+0x08 -> camera, camera+0xA0).
//
// Render (GPU command thread, through the SDK's native guest-output hook):
// meshes are decoded once into host buffers (big-endian positions and 16-bit
// indices swapped) and drawn with the camera matrix. Static world meshes are
// already in world space.
//
// The mesh, vertex-descriptor and buffer layouts follow Skate 3 Recomp's
// verified offsets (github.com/mchughalex/skate3recomp, by mchughalex, used
// with the author's permission); they were checked against Skate 2 on the
// live game. The MeshContext draw list differs: count u32 at +0x18, records
// at +0x30 (Skate 3: u16 at +0x38, records at +0x48).

#include "native_world.h"
#include "native_draw_state.h"
#include "native_texture.h"

#include "native/native_shaders_spirv.h"

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/graphics/native_rhi.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_INT32(skate2_native_lm_mode, 0, "Skate2",
                     "Native world shading: 0 the game's world shader, 1 diffuse only, "
                     "2 lightmap intensity, 3 chromaticity, 4 game state found (green) or not "
                     "(red), 5 material bump factor, 6 macro overlay, 7 sun specular, "
                     "8 world normal, 9 environment reflection")
    .range(0, 9)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate2_native_lens, true, "Skate2",
                    "Native world: the game's lens pass (barrel distortion and vignette)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate2_native_world, false, "Skate2",
                    "Experimental: draw the world natively (untextured, first stage)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace nrhi = rex::graphics::nrhi;

// Guest structure offsets (see the header comment).
constexpr uint32_t kViewDrawList = 20160;  // view: scene draw list (8-byte entries)
constexpr uint32_t kViewCamera = 0x08;      // view -> camera
constexpr uint32_t kCameraViewProj = 0xA0;  // camera: 4x4 view-projection
constexpr uint32_t kCtxDrawCount = 0x18;    // MeshContext: u32 record count
constexpr uint32_t kCtxDrawRecords = 0x30;  // MeshContext -> 16-byte records
constexpr uint32_t kMeshMaterial = 0x24;
constexpr uint32_t kMeshVertexDescriptor = 0x28;
constexpr uint32_t kMeshIndexBuffer = 0x30;
constexpr uint32_t kMeshVertexBuffer = 0x34;
constexpr uint32_t kBufferAddress = 0x18;   // +0x18 address, +0x20 size

constexpr uint32_t kMaxRecords = 8;
constexpr uint32_t kConstantFloats = 64;  // world.hlsl cbuffer C (the native RHI maximum)
constexpr uint32_t kEntityConstantFloats = 36;  // entity.hlsl cbuffer C
constexpr uint32_t kEntityVertexBytes = 52;     // pos3 uv2 normal3 weights4 bones(u8x4)
constexpr uint32_t kPaletteSlotBytes = 4096;     // entity.hlsl Palette, aligned
constexpr uint32_t kPaletteBoneRows = 216;       // 72 bones, then sh[9] and light[4]
constexpr uint32_t kPaletteSlotsPerBlock = 128;
constexpr uint32_t kPaletteBlocks = 4;
constexpr uint32_t kMaxBuildsPerFrame = 96;
constexpr uint32_t kMaxVertexBytes = 8u << 20;

struct DrawRecord {
  uint32_t prim, base_vertex, start_index, index_count;
};

struct Entry {
  uint32_t mesh = 0;
  uint32_t record_count = 0;
  DrawRecord records[kMaxRecords];
};

struct Frame {
  uint32_t view = 0;
  float view_proj[16] = {};
  float eye[3] = {};  // camera position, solved from view_proj
  std::vector<Entry> entries;
  std::vector<Entry> entities;  // RenderMesh draws (skater, pedestrians, props)
};

std::mutex g_frame_mutex;
Frame g_building;
Frame g_published;
std::atomic<uint8_t*> g_base{nullptr};
// Set by the renderer when the world flag file forces native rendering.
std::atomic<bool> g_capture_forced{false};

// ---- guarded guest memory access -------------------------------------------

// The eye is the point the view-projection maps to clip x = y = w = 0 (row
// vectors, clip_c = sum_r v_r * M[r][c]): three linear equations, Cramer's rule.
void SolveEye(const float* m, float* eye) {
  const int cols[3] = {0, 1, 3};
  float a[3][3], rhs[3];
  for (int k = 0; k < 3; ++k) {
    for (int r = 0; r < 3; ++r) a[k][r] = m[r * 4 + cols[k]];
    rhs[k] = -m[12 + cols[k]];
  }
  auto det3 = [](float (*x)[3]) {
    return x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) -
           x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
           x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
  };
  const float d = det3(a);
  if (std::fabs(d) < 1e-12f) return;
  for (int i = 0; i < 3; ++i) {
    float t[3][3];
    std::memcpy(t, a, sizeof(t));
    for (int k = 0; k < 3; ++k) t[k][i] = rhs[k];
    eye[i] = det3(t) / d;
  }
}

uint8_t* Host(uint8_t* base, uint32_t addr) {
  return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
}

// Byte-wise copy under SEH: a stale pointer (world streaming) fails the copy
// instead of crashing the game.
bool TryRead(uint8_t* base, uint32_t addr, void* out, uint32_t size) {
  if (addr < 0x10000 || uint64_t(addr) + size > 0x100000000ull) return false;
  const volatile uint8_t* src = Host(base, addr);
  uint8_t* dst = static_cast<uint8_t*>(out);
  __try {
    for (uint32_t i = 0; i < size; ++i) dst[i] = src[i];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint32_t ReadU32(uint8_t* base, uint32_t addr, bool* ok = nullptr) {
  uint8_t b[4];
  const bool good = TryRead(base, addr, b, 4);
  if (ok) *ok = good;
  return good ? Be32(b) : 0;
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      exp = 127 - 15 + 1;
      while (!(mant & 0x400)) {
        mant <<= 1;
        --exp;
      }
      bits = sign | (exp << 23) | ((mant & 0x3FF) << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000 | (mant << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// ---- GPU state (command thread only) ----------------------------------------

// position, texcoord 0, texcoord 1 (lightmap), normal, tangent + binormal sign
constexpr uint32_t kVertexFloats = 14;
constexpr uint32_t kMaxTextureDecodesPerFrame = 48;

struct MeshGpu {
  nrhi::Buffer* vb = nullptr;
  nrhi::Buffer* ib = nullptr;
  uint32_t vb_bytes = 0;
  uint32_t ib_bytes = 0;
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint32_t diffuse = 0;   // guest renderengine::Texture (0 = none)
  uint32_t lightmap = 0;  // guest renderengine::Texture (0 = none)
  uint32_t chroma = 0;    // guest renderengine::Texture (0 = none)
  uint32_t normal = 0;    // material channels of the main world material
  uint32_t detail = 0;
  uint32_t macro = 0;
  uint32_t specular = 0;
  uint32_t environment = 0;  // cube
  uint32_t lightmap_channel = 1;  // which lightmap channel holds this mesh's light
  // Guest physical addresses, the keys of the emulated draw state.
  uint32_t vb_phys = 0;
  uint32_t ib_phys = 0;
  bool skinned = false;  // entity mesh with bone indices and weights
  bool failed = false;
};

struct TextureGpu {
  NativeTexture tex;
  uint32_t fetch[6] = {};  // the fetch constant it was decoded from
  bool failed = false;
};

struct Gpu {
  nrhi::Device* device = nullptr;
  nrhi::BindingLayout* layout = nullptr;
  nrhi::Shader* vs = nullptr;
  nrhi::Shader* ps = nullptr;
  // [cull][blend][depth write], built on first use (NativeDrawState enums).
  nrhi::Pipeline* pipelines[3][3][2] = {};
  // Entities (RenderMesh): own layout, shaders and pipelines; bone palettes
  // in a ring of upload memory, one block per frame in flight.
  nrhi::BindingLayout* entity_layout = nullptr;
  nrhi::Shader* entity_vs = nullptr;
  nrhi::Shader* entity_ps = nullptr;
  nrhi::Pipeline* entity_pipelines[3][3][2] = {};
  nrhi::Pipeline* entity_shadow_pipeline = nullptr;
  nrhi::Texture* shadow = nullptr;  // sun shadow atlas
  nrhi::TextureView* shadow_view = nullptr;
  nrhi::Buffer* palette = nullptr;
  uint8_t* palette_map = nullptr;
  uint64_t palette_block_submission[4] = {};
  uint32_t palette_frame = 0;
  std::unordered_map<uint32_t, MeshGpu> entity_meshes;
  nrhi::Format color_format = nrhi::Format::kUnknown;
  nrhi::Texture* depth = nullptr;
  uint32_t width = 0, height = 0;
  nrhi::Texture* white = nullptr;
  nrhi::TextureView* white_view = nullptr;
  std::unordered_map<uint32_t, MeshGpu> meshes;
  std::unordered_map<uint32_t, TextureGpu> textures;
  // Environment cubes as their six face averages (a 6x1 texture each).
  std::unordered_map<uint32_t, TextureGpu> env_cubes;
  uint64_t frames = 0;
  uint32_t failed_meshes = 0;
  uint32_t failed_textures = 0;
  uint32_t redecoded_textures = 0;
  // Lens pass (EnsurePost): the world is drawn into `scene` first.
  nrhi::BindingLayout* post_layout = nullptr;
  nrhi::Shader* post_vs = nullptr;
  nrhi::Shader* post_ps = nullptr;
  nrhi::Pipeline* post_pipeline = nullptr;
  nrhi::Format post_format = nrhi::Format::kUnknown;
  nrhi::Texture* scene = nullptr;
  nrhi::TextureView* scene_view = nullptr;
  uint32_t scene_width = 0, scene_height = 0;
  nrhi::Format scene_format = nrhi::Format::kUnknown;
  NativeTexture vignette;
  uint32_t vignette_fetch[6] = {};
  bool vignette_tried = false;
  NativeTexture lut;  // grading table atlas; width = table size n
  uint32_t lut_fetch[6] = {};
  bool lut_tried = false;
} g;

nrhi::Buffer* UploadBuffer(nrhi::Device* device, const void* data, uint32_t bytes) {
  nrhi::BufferDesc desc;
  desc.size = bytes;
  desc.heap = nrhi::HeapKind::kUpload;
  desc.bind_class = nrhi::BufferBindClass::kVertexIndex;
  nrhi::Buffer* buffer = device->CreateBuffer(desc);
  if (!buffer) return nullptr;
  void* mapped = device->Map(buffer);
  if (!mapped) {
    device->DestroyDeferred(buffer);
    return nullptr;
  }
  std::memcpy(mapped, data, bytes);
  device->Unmap(buffer);
  return buffer;
}

// Texture coordinate decode (Skate 3 Recomp's decode_uv): 31/32 half float,
// 37/38 float, 25/26 s16 snorm.
void DecodeUv(const uint8_t* q, uint32_t fmt, float& u, float& v) {
  u = v = 0.0f;
  switch (fmt) {
    case 31:
    case 32:
      u = HalfToFloat(uint16_t(q[0] << 8 | q[1]));
      v = HalfToFloat(uint16_t(q[2] << 8 | q[3]));
      break;
    case 37:
    case 38: {
      const uint32_t a = Be32(q), b = Be32(q + 4);
      std::memcpy(&u, &a, 4);
      std::memcpy(&v, &b, 4);
      break;
    }
    case 25:
    case 26:
      u = int16_t(q[0] << 8 | q[1]) / 32767.0f;
      v = int16_t(q[2] << 8 | q[3]) / 32767.0f;
      break;
    default:
      break;
  }
}

// Material channels (same layout as Skate 3): material+0 count, +8 -> 0x20-byte
// channels: +0 name, +0x10 asset GUID of the texture.
void ResolveMaterial(uint8_t* base, uint32_t mesh, MeshGpu& out) {
  const uint32_t material = ReadU32(base, mesh + kMeshMaterial);
  const uint32_t count = ReadU32(base, material);
  const uint32_t channels = ReadU32(base, material + 8);
  if (!material || count == 0 || count > 32 || !channels) return;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t ch = channels + i * 0x20;
    char name[16] = {};
    if (!TryRead(base, ReadU32(base, ch), name, sizeof(name) - 1)) continue;
    uint32_t* slot = nullptr;
    if (std::memcmp(name, "diffuse", 8) == 0) {
      slot = &out.diffuse;
    } else if (std::memcmp(name, "lightmap", 9) == 0) {
      slot = &out.lightmap;
    } else if (std::memcmp(name, "chromaticity", 13) == 0) {
      slot = &out.chroma;
    } else if (std::memcmp(name, "normal", 7) == 0) {
      slot = &out.normal;
    } else if (std::memcmp(name, "detail", 7) == 0) {
      slot = &out.detail;
    } else if (std::memcmp(name, "macrooverlay", 13) == 0) {
      slot = &out.macro;
    } else if (std::memcmp(name, "specular", 9) == 0) {
      slot = &out.specular;
    } else if (std::memcmp(name, "environment", 12) == 0) {
      slot = &out.environment;
    }
    if (!slot || *slot) continue;
    // Lightmaps pack up to four meshes' intensity maps, one per channel; the
    // material channel's +4 is the index (matches the one-hot VS c7 the game
    // sets for each world draw).
    if (slot == &out.lightmap) out.lightmap_channel = ReadU32(base, ch + 4) & 3;
    const uint64_t guid = uint64_t(ReadU32(base, ch + 0x10)) << 32 | ReadU32(base, ch + 0x14);
    *slot = NativeWorldTextureForGuid(guid);
  }
}

// Decode a guest mesh (vertex descriptor, positions, texcoords, indices) into
// host buffers. Vertex descriptor: element count u16 at +8, 16-byte elements
// from +0x10 (stream u16 @0, offset u16 @2, type u32 @4 with the Xenos format
// in the low 6 bits, usage u8 @9: 0 position, 5 texcoord), the stride byte
// after the last element.
void Unpack101111(uint32_t word, float out[3]);

MeshGpu BuildMesh(uint8_t* base, uint32_t mesh) {
  MeshGpu out;
  out.failed = true;
  const uint32_t vdesc = ReadU32(base, mesh + kMeshVertexDescriptor);
  const uint32_t ib = ReadU32(base, mesh + kMeshIndexBuffer);
  const uint32_t vb = ReadU32(base, mesh + kMeshVertexBuffer);
  uint8_t head[12];
  if (!vdesc || !ib || !vb || !TryRead(base, vdesc, head, sizeof(head))) return out;
  const uint32_t elements = Be32(head + 8) >> 16;
  if (elements == 0 || elements > 32) return out;
  uint8_t table[32 * 16 + 1];
  if (!TryRead(base, vdesc + 0x10, table, elements * 16 + 1)) return out;
  int pos_offset = -1, uv_offset = -1, uv2_offset = -1, tangent_offset = -1;
  uint32_t pos_format = 0, uv_format = 0, uv2_format = 0;
  for (uint32_t i = 0; i < elements; ++i) {
    const uint8_t* e = table + i * 16;
    const uint32_t stream = uint32_t(e[0]) << 8 | e[1];
    if (stream != 0) continue;
    const int offset = int(uint32_t(e[2]) << 8 | e[3]);
    const uint32_t format = Be32(e + 4) & 0x3F;
    if (e[9] == 0 && pos_offset < 0) {
      pos_offset = offset;
      pos_format = format;
    } else if (e[9] == 5 && uv_offset < 0) {
      uv_offset = offset;
      uv_format = format;
    } else if (e[9] == 5 && uv2_offset < 0) {
      uv2_offset = offset;
      uv2_format = format;
    } else if (e[9] == 6 && tangent_offset < 0 && format == 16) {
      tangent_offset = offset;
    }
  }
  const uint32_t stride = table[elements * 16];
  if (pos_offset < 0 || stride == 0 || (pos_format != 57 && pos_format != 32)) return out;

  uint8_t vb_words[12], ib_words[12];
  if (!TryRead(base, vb + kBufferAddress, vb_words, 12) ||
      !TryRead(base, ib + kBufferAddress, ib_words, 12)) {
    return out;
  }
  const uint32_t vb_addr = Be32(vb_words) & 0xFFFFFFFCu;
  const uint32_t vb_bytes = Be32(vb_words + 8);
  const uint32_t ib_addr = Be32(ib_words) & 0xFFFFFFFCu;
  const uint32_t ib_count = Be32(ib_words + 8);
  if (!vb_addr || !ib_addr || !vb_bytes || !ib_count || vb_bytes % stride ||
      vb_bytes > kMaxVertexBytes || ib_count > (kMaxVertexBytes / 2)) {
    return out;
  }
  const uint32_t vertex_count = vb_bytes / stride;
  std::vector<uint8_t> raw(vb_bytes);
  if (!TryRead(base, vb_addr, raw.data(), vb_bytes)) return out;
  std::vector<float> verts(size_t(vertex_count) * kVertexFloats);
  for (uint32_t v = 0; v < vertex_count; ++v) {
    const uint8_t* vert = raw.data() + size_t(v) * stride;
    float* dst = &verts[size_t(v) * kVertexFloats];
    const uint8_t* p = vert + pos_offset;
    for (int c = 0; c < 3; ++c) {
      if (pos_format == 57) {
        const uint32_t bits = Be32(p + c * 4);
        std::memcpy(&dst[c], &bits, 4);
      } else {
        dst[c] = HalfToFloat(uint16_t(p[c * 2] << 8 | p[c * 2 + 1]));
      }
    }
    if (uv_offset >= 0) DecodeUv(vert + uv_offset, uv_format, dst[3], dst[4]);
    if (uv2_offset >= 0) {
      // The lightmap unwrap stores |uv| with tangent handedness in the sign
      // bits; raw signed values sample mirrored atlas cells (Skate 3 Recomp).
      DecodeUv(vert + uv2_offset, uv2_format, dst[5], dst[6]);
      dst[5] = std::fabs(dst[5]);
      dst[6] = std::fabs(dst[6]);
      if (uv2_format == 26) {
        // The world material's vertex shader (E5C957221BEF0ECD): the same
        // k_16_16_16_16 element carries the normal's x, y in z, w; the normal's
        // z sign rides the sign of v and the binormal's sign that of u.
        const uint8_t* q = vert + uv2_offset;
        const float u = int16_t(q[0] << 8 | q[1]) / 32767.0f;
        const float lv = int16_t(q[2] << 8 | q[3]) / 32767.0f;
        const float nx = int16_t(q[4] << 8 | q[5]) / 32767.0f;
        const float ny = int16_t(q[6] << 8 | q[7]) / 32767.0f;
        const float nz = std::sqrt(1.0f - std::min(nx * nx + ny * ny, 0.999f));
        dst[7] = nx;
        dst[8] = ny;
        dst[9] = lv > 0.0f ? nz : -nz;
        dst[13] = u > 0.0f ? 1.0f : -1.0f;
      }
    }
    if (tangent_offset >= 0) Unpack101111(Be32(vert + tangent_offset), dst + 10);
  }
  std::vector<uint16_t> indices(ib_count);
  if (!TryRead(base, ib_addr, indices.data(), ib_count * 2)) return out;
  for (uint16_t& i : indices) i = uint16_t(i >> 8 | i << 8);

  out.vb = UploadBuffer(g.device, verts.data(), uint32_t(verts.size() * 4));
  out.ib = UploadBuffer(g.device, indices.data(), ib_count * 2);
  if (!out.vb || !out.ib) return out;
  out.vb_bytes = uint32_t(verts.size() * 4);
  out.ib_bytes = ib_count * 2;
  out.vertex_count = vertex_count;
  out.index_count = ib_count;
  out.vb_phys = vb_addr & 0x1FFFFFFCu;
  out.ib_phys = ib_addr & 0x1FFFFFFFu;
  ResolveMaterial(base, mesh, out);
  out.failed = false;
  return out;
}

// ---- entities (RenderMesh) --------------------------------------------------

nrhi::Pipeline* GetPipeline(nrhi::Format color_format, const NativeDrawState& state);

// Signed k_10_11_11 (x 11 bits, y 11 bits, z 10 bits from the LSB), as the
// game's vertex shaders fetch normals, tangents and binormals.
void Unpack101111(uint32_t word, float out[3]) {
  out[0] = float(int32_t(word << 21) >> 21) / 1023.0f;
  out[1] = float(int32_t((word >> 11) << 21) >> 21) / 1023.0f;
  out[2] = float(int32_t(word) >> 22) / 511.0f;
}

// Entity mesh: position (k_16_16_16_16 character dequantisation, half or
// float), first texcoord, normal (stored, or tangent x binormal), bone weights
// and indices. Vertex: pos3 uv2 normal3 weights4 (floats) + bones (u8x4).
MeshGpu BuildEntityMesh(uint8_t* base, uint32_t mesh) {
  MeshGpu out;
  out.failed = true;
  const uint32_t vdesc = ReadU32(base, mesh + kMeshVertexDescriptor);
  const uint32_t ib = ReadU32(base, mesh + kMeshIndexBuffer);
  const uint32_t vb = ReadU32(base, mesh + kMeshVertexBuffer);
  uint8_t head[12];
  if (!vdesc || !ib || !vb || !TryRead(base, vdesc, head, sizeof(head))) return out;
  const uint32_t elements = Be32(head + 8) >> 16;
  if (elements == 0 || elements > 32) return out;
  uint8_t table[32 * 16 + 1];
  if (!TryRead(base, vdesc + 0x10, table, elements * 16 + 1)) return out;
  // D3DDECLUSAGE: 0 position, 1 blend weight, 2 blend indices, 3 normal,
  // 5 texcoord, 6 tangent, 7 binormal.
  int offset_of[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  uint32_t format_of[8] = {};
  for (uint32_t i = 0; i < elements; ++i) {
    const uint8_t* e = table + i * 16;
    if ((uint32_t(e[0]) << 8 | e[1]) != 0) continue;
    const uint32_t usage = e[9];
    if (usage < 8 && offset_of[usage] < 0) {
      offset_of[usage] = int(uint32_t(e[2]) << 8 | e[3]);
      format_of[usage] = Be32(e + 4) & 0x3F;
    }
  }
  const uint32_t stride = table[elements * 16];
  const uint32_t pos_format = format_of[0];
  if (offset_of[0] < 0 || stride == 0 ||
      (pos_format != 26 && pos_format != 32 && pos_format != 57)) {
    return out;
  }
  const bool skinned = offset_of[1] >= 0 && offset_of[2] >= 0 && format_of[1] == 6 &&
                       format_of[2] == 6;

  uint8_t vb_words[12], ib_words[12];
  if (!TryRead(base, vb + kBufferAddress, vb_words, 12) ||
      !TryRead(base, ib + kBufferAddress, ib_words, 12)) {
    return out;
  }
  const uint32_t vb_addr = Be32(vb_words) & 0xFFFFFFFCu;
  const uint32_t vb_bytes = Be32(vb_words + 8);
  const uint32_t ib_addr = Be32(ib_words) & 0xFFFFFFFCu;
  const uint32_t ib_count = Be32(ib_words + 8);
  if (!vb_addr || !ib_addr || !vb_bytes || !ib_count || vb_bytes % stride ||
      vb_bytes > kMaxVertexBytes || ib_count > (kMaxVertexBytes / 2)) {
    return out;
  }
  const uint32_t vertex_count = vb_bytes / stride;
  std::vector<uint8_t> raw(vb_bytes);
  if (!TryRead(base, vb_addr, raw.data(), vb_bytes)) return out;
  std::vector<uint8_t> verts(size_t(vertex_count) * kEntityVertexBytes);
  for (uint32_t v = 0; v < vertex_count; ++v) {
    const uint8_t* vert = raw.data() + size_t(v) * stride;
    float f[12] = {};
    const uint8_t* p = vert + offset_of[0];
    for (int c = 0; c < 3; ++c) {
      if (pos_format == 57) {
        const uint32_t bits = Be32(p + c * 4);
        std::memcpy(&f[c], &bits, 4);
      } else if (pos_format == 32) {
        f[c] = HalfToFloat(uint16_t(p[c * 2] << 8 | p[c * 2 + 1]));
      } else {
        f[c] = int16_t(p[c * 2] << 8 | p[c * 2 + 1]) * (2.0f / 32767.0f);
      }
    }
    if (pos_format == 26) f[1] += 0.8f;
    if (offset_of[5] >= 0) DecodeUv(vert + offset_of[5], format_of[5], f[3], f[4]);
    if (offset_of[3] >= 0 && format_of[3] == 16) {
      Unpack101111(Be32(vert + offset_of[3]), f + 5);
    } else if (offset_of[6] >= 0 && offset_of[7] >= 0 && format_of[6] == 16 &&
               format_of[7] == 16) {
      float t[3], b[3];
      Unpack101111(Be32(vert + offset_of[6]), t);
      Unpack101111(Be32(vert + offset_of[7]), b);
      f[5] = t[1] * b[2] - t[2] * b[1];
      f[6] = t[2] * b[0] - t[0] * b[2];
      f[7] = t[0] * b[1] - t[1] * b[0];
    }
    uint32_t bones = 0;
    if (skinned) {
      // u8x4 components run from the low byte of the big-endian word; the
      // game blends x, y, z and gives the fourth bone 1 - (x + y + z).
      const uint32_t w = Be32(vert + offset_of[1]);
      f[8] = float(w & 0xFF) / 255.0f;
      f[9] = float((w >> 8) & 0xFF) / 255.0f;
      f[10] = float((w >> 16) & 0xFF) / 255.0f;
      f[11] = 1.0f - f[8] - f[9] - f[10];
      bones = Be32(vert + offset_of[2]);
    }
    uint8_t* dst = &verts[size_t(v) * kEntityVertexBytes];
    std::memcpy(dst, f, sizeof(f));
    std::memcpy(dst + 48, &bones, 4);
  }
  std::vector<uint16_t> indices(ib_count);
  if (!TryRead(base, ib_addr, indices.data(), ib_count * 2)) return out;
  for (uint16_t& i : indices) i = uint16_t(i >> 8 | i << 8);

  out.vb = UploadBuffer(g.device, verts.data(), uint32_t(verts.size()));
  out.ib = UploadBuffer(g.device, indices.data(), ib_count * 2);
  if (!out.vb || !out.ib) return out;
  out.vb_bytes = uint32_t(verts.size());
  out.ib_bytes = ib_count * 2;
  out.vertex_count = vertex_count;
  out.index_count = ib_count;
  out.vb_phys = vb_addr & 0x1FFFFFFCu;
  out.ib_phys = ib_addr & 0x1FFFFFFFu;
  out.skinned = skinned;
  ResolveMaterial(base, mesh, out);
  out.failed = false;
  return out;
}

// Character pixel shaders: where their lighting constants live. The game's
// character shading (ucode of 3BE9E8635A346045): light = max(SH9(N), floor) *
// ambient scale + fill + sun colour * saturate(N.L) * shadow; colour =
// diffuse^2 * light + specular, fog, the tone curve with its own exposure.
struct CharacterLayout {
  uint64_t pixel_shader;
  uint8_t sun_dir, exposure, sun_color, ambient_scale, fill, sh, floor;
};
constexpr CharacterLayout kCharacterLayouts[] = {
    {0x3BE9E8635A346045ull, 0, 4, 6, 10, 11, 13, 253},
    {0x305E195F3A92EE70ull, 3, 7, 9, 13, 14, 16, 255},
    {0x7F1B252B79C1AD22ull, 3, 7, 9, 13, 14, 16, 255},
};

// Writes sh[9] and light[4] (entity.hlsl Palette) from the draw's pixel
// shader constants; false for shaders not in the table.
bool WriteCharacterLighting(const NativeDrawConstants& dc, float* out) {
  for (const CharacterLayout& l : kCharacterLayouts) {
    if (l.pixel_shader != dc.pixel_shader || !dc.ps) continue;
    const float* c = dc.ps;
    std::memcpy(out, c + l.sh * 4, 9 * 4 * sizeof(float));
    float* light = out + 9 * 4;
    std::memcpy(light, c + l.sun_dir * 4, 3 * sizeof(float));
    light[3] = c[l.exposure * 4 + 2];
    std::memcpy(light + 4, c + l.sun_color * 4, 3 * sizeof(float));
    light[7] = c[l.ambient_scale * 4 + 3];
    std::memcpy(light + 8, c + l.fill * 4, 4 * sizeof(float));
    std::memcpy(light + 12, c + l.floor * 4, 4 * sizeof(float));
    return true;
  }
  return false;
}

// First palette row of the game's skinned vertex shaders (c7 unless listed).
uint32_t PaletteBase(uint64_t vertex_shader) {
  return vertex_shader == 0x8845ED4E9481A4BFull ? 8 : 7;
}

bool EnsureEntityShaders() {
  if (g.entity_layout && g.entity_vs && g.entity_ps && g.palette_map) return true;
  if (!g.entity_layout) {
    nrhi::BindingLayoutDesc layout;
    layout.params[0].kind = nrhi::BindingParamKind::kConstants;
    layout.params[0].shader_register = 0;
    layout.params[0].count = kEntityConstantFloats;
    layout.params[1].kind = nrhi::BindingParamKind::kConstantBuffer;
    layout.params[1].shader_register = 1;
    layout.params[1].visibility = nrhi::Visibility::kVertex;
    layout.params[2].kind = nrhi::BindingParamKind::kTextureTable;
    layout.params[2].shader_register = 0;
    layout.params[2].count = 1;
    layout.params[2].visibility = nrhi::Visibility::kPixel;
    layout.param_count = 3;
    layout.static_samplers[0].shader_register = 0;
    // The game's world and character samplers: trilinear, anisotropic 4:1
    // (texture fetch constants of its draws). nrhi enables anisotropy only
    // for Filter::kAnisotropic.
    layout.static_samplers[0].filter = nrhi::Filter::kAnisotropic;
    layout.static_samplers[0].address = nrhi::AddressMode::kWrap;
    layout.static_samplers[0].max_anisotropy = 4;
    layout.static_sampler_count = 1;
    g.entity_layout = g.device->CreateBindingLayout(layout);
    namespace sh = skate2::native_shaders;
    nrhi::ShaderDesc vs;
    vs.stage = nrhi::ShaderStage::kVertex;
    vs.name = "entity_vs";
    vs.hlsl_source = sh::kEntityHlsl;
    vs.entry_point = "vs_main";
    vs.spirv = sh::kEntityVs;
    vs.spirv_size_bytes = sizeof(sh::kEntityVs);
    g.entity_vs = g.device->CreateShader(vs);
    nrhi::ShaderDesc ps = vs;
    ps.stage = nrhi::ShaderStage::kPixel;
    ps.name = "entity_ps";
    ps.entry_point = "ps_main";
    ps.spirv = sh::kEntityPs;
    ps.spirv_size_bytes = sizeof(sh::kEntityPs);
    g.entity_ps = g.device->CreateShader(ps);
  }
  if (!g.palette) {
    nrhi::BufferDesc desc;
    // The UBO binding reads a 16 KiB window from each slot offset.
    desc.size = uint64_t(kPaletteSlotBytes) * kPaletteSlotsPerBlock * kPaletteBlocks + 16384;
    desc.heap = nrhi::HeapKind::kUpload;
    g.palette = g.device->CreateBuffer(desc);
    if (g.palette) g.palette_map = static_cast<uint8_t*>(g.device->Map(g.palette));
  }
  return g.entity_layout && g.entity_vs && g.entity_ps && g.palette_map;
}

nrhi::Pipeline* GetEntityPipeline(nrhi::Format color_format, const NativeDrawState& state) {
  // GetPipeline owns the colour format and drops every variant when it changes.
  if (g.color_format != color_format && !GetPipeline(color_format, state)) return nullptr;
  nrhi::Pipeline*& slot = g.entity_pipelines[state.cull][state.blend][state.depth_write ? 1 : 0];
  if (slot) return slot;
  nrhi::InputElementDesc inputs[5];
  inputs[0].semantic_name = "POSITION";
  inputs[0].location = 0;
  inputs[0].format = nrhi::Format::kR32G32B32_FLOAT;
  inputs[1].semantic_name = "TEXCOORD";
  inputs[1].location = 1;
  inputs[1].format = nrhi::Format::kR32G32_FLOAT;
  inputs[1].byte_offset = 12;
  inputs[2].semantic_name = "NORMAL";
  inputs[2].location = 2;
  inputs[2].format = nrhi::Format::kR32G32B32_FLOAT;
  inputs[2].byte_offset = 20;
  inputs[3].semantic_name = "BLENDWEIGHT";
  inputs[3].location = 3;
  inputs[3].format = nrhi::Format::kR32G32B32A32_FLOAT;
  inputs[3].byte_offset = 32;
  inputs[4].semantic_name = "BLENDINDICES";
  inputs[4].location = 4;
  inputs[4].format = nrhi::Format::kR8G8B8A8_UINT;
  inputs[4].byte_offset = 48;
  nrhi::GraphicsPipelineDesc desc;
  desc.layout = g.entity_layout;
  desc.vs = g.entity_vs;
  desc.ps = g.entity_ps;
  desc.input_elements = inputs;
  desc.input_element_count = 5;
  desc.vertex_stride = kEntityVertexBytes;
  desc.depth.test_enable = true;
  desc.depth.write_enable = state.depth_write;
  desc.depth.func = nrhi::CompareFunc::kLessEqual;
  desc.cull = state.cull == NativeDrawState::kFront  ? nrhi::CullMode::kFront
              : state.cull == NativeDrawState::kBack ? nrhi::CullMode::kBack
                                                     : nrhi::CullMode::kNone;
  if (state.blend == NativeDrawState::kAlpha) {
    desc.blend.enable = true;
    desc.blend.src = nrhi::BlendFactor::kSrcAlpha;
    desc.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  } else if (state.blend == NativeDrawState::kAdditive) {
    desc.blend.enable = true;
    desc.blend.src = nrhi::BlendFactor::kOne;
    desc.blend.dst = nrhi::BlendFactor::kOne;
  }
  desc.blend.write_mask = 0x7;
  desc.rtv_format = color_format;
  desc.dsv_format = nrhi::Format::kD32_FLOAT;
  slot = g.device->CreateGraphicsPipeline(desc);
  return slot;
}

// The sun shadow map: a 1024x512 depth atlas with the game's two cascades
// side by side (left: X, Y; right: c1-scaled), drawn from the entities (the
// game's own shadow pass at pitch 560 is suppressed). Depth is the game's
// light depth c3.p stored as 0.5 + 0.05 z.
constexpr uint32_t kShadowWidth = 1024, kShadowHeight = 512;

bool EnsureShadow() {
  if (g.shadow) return g.shadow_view != nullptr;
  nrhi::TextureDesc desc;
  desc.width = kShadowWidth;
  desc.height = kShadowHeight;
  desc.format = nrhi::Format::kD32_FLOAT;
  desc.usage = nrhi::kTextureUsageDepthStencil;
  desc.initial_state = nrhi::ResourceState::kPixelShaderResource;
  desc.clear_depth = 1.0f;
  g.shadow = g.device->CreateTexture(desc);
  if (!g.shadow) return false;
  nrhi::TextureViewDesc view;
  g.shadow_view = g.device->CreateTextureView(g.shadow, view);
  return g.shadow_view != nullptr;
}

nrhi::Pipeline* GetEntityShadowPipeline() {
  if (g.entity_shadow_pipeline) return g.entity_shadow_pipeline;
  nrhi::InputElementDesc inputs[5];
  inputs[0].semantic_name = "POSITION";
  inputs[0].location = 0;
  inputs[0].format = nrhi::Format::kR32G32B32_FLOAT;
  inputs[1].semantic_name = "TEXCOORD";
  inputs[1].location = 1;
  inputs[1].format = nrhi::Format::kR32G32_FLOAT;
  inputs[1].byte_offset = 12;
  inputs[2].semantic_name = "NORMAL";
  inputs[2].location = 2;
  inputs[2].format = nrhi::Format::kR32G32B32_FLOAT;
  inputs[2].byte_offset = 20;
  inputs[3].semantic_name = "BLENDWEIGHT";
  inputs[3].location = 3;
  inputs[3].format = nrhi::Format::kR32G32B32A32_FLOAT;
  inputs[3].byte_offset = 32;
  inputs[4].semantic_name = "BLENDINDICES";
  inputs[4].location = 4;
  inputs[4].format = nrhi::Format::kR8G8B8A8_UINT;
  inputs[4].byte_offset = 48;
  nrhi::GraphicsPipelineDesc desc;
  desc.layout = g.entity_layout;
  desc.vs = g.entity_vs;
  desc.ps = g.entity_ps;
  desc.input_elements = inputs;
  desc.input_element_count = 5;
  desc.vertex_stride = kEntityVertexBytes;
  desc.depth.test_enable = true;
  desc.depth.write_enable = true;
  desc.depth.func = nrhi::CompareFunc::kLessEqual;
  desc.cull = nrhi::CullMode::kNone;
  desc.depth_clip = false;
  desc.rtv_format = nrhi::Format::kUnknown;
  desc.dsv_format = nrhi::Format::kD32_FLOAT;
  g.entity_shadow_pipeline = g.device->CreateGraphicsPipeline(desc);
  return g.entity_shadow_pipeline;
}

bool EnsureWhite(nrhi::Cmd* cmd) {
  if (g.white_view) return true;
  nrhi::TextureDesc desc;
  desc.width = 4;
  desc.height = 4;
  desc.format = nrhi::Format::kR8G8B8A8_UNORM;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  g.white = g.device->CreateTexture(desc);
  if (!g.white) return false;
  std::vector<uint8_t> pixels(nrhi::kRowPitchAlignment * 4, 0xFF);
  nrhi::BufferDesc up;
  up.size = pixels.size();
  up.heap = nrhi::HeapKind::kUpload;
  up.bind_class = nrhi::BufferBindClass::kCopySrc;
  nrhi::Buffer* upload = g.device->CreateBuffer(up);
  if (!upload) return false;
  std::memcpy(g.device->Map(upload), pixels.data(), pixels.size());
  g.device->Unmap(upload);
  cmd->CopyBufferToTexture(g.white, 0, 0, upload, 0, nrhi::kRowPitchAlignment, 4, 4, 1);
  cmd->Barrier(g.white, nrhi::ResourceState::kCopyDest, nrhi::ResourceState::kPixelShaderResource);
  g.device->DestroyDeferred(upload);
  nrhi::TextureViewDesc view;
  g.white_view = g.device->CreateTextureView(g.white, view);
  return g.white_view != nullptr;
}

// Environment cube -> 6x1 RGBA8 texture of its face averages, built on first
// use (shares the decode budget); nullptr while unavailable or not a cube.
nrhi::TextureView* EnvironmentView(nrhi::Cmd* cmd, uint8_t* base, uint32_t texture,
                                   uint32_t& budget) {
  if (!texture) return nullptr;
  auto it = g.env_cubes.find(texture);
  if (it == g.env_cubes.end()) {
    if (budget == 0) return nullptr;
    --budget;
    TextureGpu t;
    float faces[6][4];
    t.failed = !NativeCubeFaceAverages(base, texture, faces);
    if (!t.failed) {
      nrhi::TextureDesc desc;
      desc.width = 6;
      desc.height = 1;
      desc.format = nrhi::Format::kR8G8B8A8_UNORM;
      desc.initial_state = nrhi::ResourceState::kCopyDest;
      t.tex.texture = g.device->CreateTexture(desc);
      nrhi::BufferDesc up;
      up.size = nrhi::kRowPitchAlignment;
      up.heap = nrhi::HeapKind::kUpload;
      up.bind_class = nrhi::BufferBindClass::kCopySrc;
      nrhi::Buffer* upload = t.tex.texture ? g.device->CreateBuffer(up) : nullptr;
      if (upload) {
        uint8_t* px = static_cast<uint8_t*>(g.device->Map(upload));
        for (int f = 0; f < 6; ++f) {
          for (int k = 0; k < 4; ++k) {
            px[f * 4 + k] = uint8_t(std::clamp(faces[f][k], 0.0f, 1.0f) * 255.0f + 0.5f);
          }
        }
        g.device->Unmap(upload);
        cmd->CopyBufferToTexture(t.tex.texture, 0, 0, upload, 0, nrhi::kRowPitchAlignment, 6, 1,
                                 1);
        cmd->Barrier(t.tex.texture, nrhi::ResourceState::kCopyDest,
                     nrhi::ResourceState::kPixelShaderResource);
        g.device->DestroyDeferred(upload);
        nrhi::TextureViewDesc view;
        t.tex.view = g.device->CreateTextureView(t.tex.texture, view);
      }
      t.failed = t.tex.view == nullptr;
    }
    it = g.env_cubes.emplace(texture, t).first;
  }
  return it->second.failed ? nullptr : it->second.tex.view;
}

// Returns the view of a decoded guest texture, decoding it on first use
// (bounded per frame by `budget`); nullptr while unavailable.
nrhi::TextureView* TextureView(nrhi::Cmd* cmd, uint8_t* base, uint32_t texture,
                               uint32_t& budget) {
  if (!texture) return nullptr;
  uint32_t fetch[6];
  if (!ReadNativeTextureFetch(base, texture, fetch)) return nullptr;
  auto it = g.textures.find(texture);
  // Streaming swaps a texture's data in place: a low-resolution placeholder
  // first, the full mips later, under the same object with a new fetch
  // constant. Decode again when it changes.
  const bool stale = it != g.textures.end() &&
                     std::memcmp(it->second.fetch, fetch, sizeof(fetch)) != 0;
  if (it == g.textures.end() || stale) {
    if (budget == 0) {
      return it != g.textures.end() && !it->second.failed ? it->second.tex.view : nullptr;
    }
    --budget;
    TextureGpu t;
    std::memcpy(t.fetch, fetch, sizeof(fetch));
    t.failed = !DecodeNativeTexture(g.device, cmd, base, fetch, t.tex);
    if (t.failed) ++g.failed_textures;
    if (stale) {
      g.device->DestroyDeferred(it->second.tex.view);
      g.device->DestroyDeferred(it->second.tex.texture);
      it->second = t;
      ++g.redecoded_textures;
    } else {
      it = g.textures.emplace(texture, t).first;
    }
  }
  return it->second.failed ? nullptr : it->second.tex.view;
}

bool EnsureShaders() {
  if (!g.layout) {
    nrhi::BindingLayoutDesc layout;
    layout.params[0].kind = nrhi::BindingParamKind::kConstants;
    layout.params[0].shader_register = 0;
    layout.params[0].count = kConstantFloats;  // see world.hlsl cbuffer C
    layout.params[1].kind = nrhi::BindingParamKind::kTextureTable;
    // t0 diffuse, t1 lightmap, t2 chromaticity, t3 normal, t4 detail,
    // t5 macro overlay, t6 specular, t7 environment (cube face averages)
    layout.params[1].shader_register = 0;
    layout.params[1].count = 8;
    layout.params[1].visibility = nrhi::Visibility::kPixel;
    layout.params[2].kind = nrhi::BindingParamKind::kTextureTable;
    layout.params[2].shader_register = 8;  // t8 sun shadow atlas
    layout.params[2].count = 1;
    layout.params[2].visibility = nrhi::Visibility::kPixel;
    layout.param_count = 3;
    layout.static_samplers[0].shader_register = 0;
    // The game's world and character samplers: trilinear, anisotropic 4:1
    // (texture fetch constants of its draws). nrhi enables anisotropy only
    // for Filter::kAnisotropic.
    layout.static_samplers[0].filter = nrhi::Filter::kAnisotropic;
    layout.static_samplers[0].address = nrhi::AddressMode::kWrap;
    layout.static_samplers[0].max_anisotropy = 4;
    layout.static_samplers[1].shader_register = 1;
    layout.static_samplers[1].filter = nrhi::Filter::kLinear;
    layout.static_samplers[1].address = nrhi::AddressMode::kClamp;
    layout.static_samplers[2].shader_register = 2;
    layout.static_samplers[2].filter = nrhi::Filter::kPoint;
    layout.static_samplers[2].address = nrhi::AddressMode::kClamp;
    layout.static_sampler_count = 3;
    g.layout = g.device->CreateBindingLayout(layout);
    namespace sh = skate2::native_shaders;
    nrhi::ShaderDesc vs;
    vs.stage = nrhi::ShaderStage::kVertex;
    vs.name = "world_vs";
    vs.hlsl_source = sh::kWorldHlsl;
    vs.entry_point = "vs_main";
    vs.spirv = sh::kWorldVs;
    vs.spirv_size_bytes = sizeof(sh::kWorldVs);
    g.vs = g.device->CreateShader(vs);
    nrhi::ShaderDesc ps = vs;
    ps.stage = nrhi::ShaderStage::kPixel;
    ps.name = "world_ps";
    ps.entry_point = "ps_main";
    ps.spirv = sh::kWorldPs;
    ps.spirv_size_bytes = sizeof(sh::kWorldPs);
    g.ps = g.device->CreateShader(ps);
  }
  return g.layout && g.vs && g.ps;
}

nrhi::Pipeline* GetPipeline(nrhi::Format color_format, const NativeDrawState& state) {
  if (g.color_format != color_format) {
    for (auto* set : {&g.pipelines, &g.entity_pipelines})
      for (auto& a : *set)
        for (auto& b : a)
          for (nrhi::Pipeline*& p : b) {
            if (p) g.device->DestroyDeferred(p);
            p = nullptr;
          }
    g.color_format = color_format;
  }
  nrhi::Pipeline*& slot = g.pipelines[state.cull][state.blend][state.depth_write ? 1 : 0];
  if (slot) return slot;
  nrhi::InputElementDesc inputs[5];
  inputs[0].semantic_name = "POSITION";
  inputs[0].location = 0;
  inputs[0].format = nrhi::Format::kR32G32B32_FLOAT;
  inputs[1].semantic_name = "TEXCOORD";
  inputs[1].semantic_index = 0;
  inputs[1].location = 1;
  inputs[1].format = nrhi::Format::kR32G32_FLOAT;
  inputs[1].byte_offset = 12;
  inputs[2].semantic_name = "TEXCOORD";
  inputs[2].semantic_index = 1;
  inputs[2].location = 2;
  inputs[2].format = nrhi::Format::kR32G32_FLOAT;
  inputs[2].byte_offset = 20;
  inputs[3].semantic_name = "NORMAL";
  inputs[3].location = 3;
  inputs[3].format = nrhi::Format::kR32G32B32_FLOAT;
  inputs[3].byte_offset = 28;
  inputs[4].semantic_name = "TANGENT";
  inputs[4].location = 4;
  inputs[4].format = nrhi::Format::kR32G32B32A32_FLOAT;
  inputs[4].byte_offset = 40;
  nrhi::GraphicsPipelineDesc desc;
  desc.layout = g.layout;
  desc.vs = g.vs;
  desc.ps = g.ps;
  desc.input_elements = inputs;
  desc.input_element_count = 5;
  desc.vertex_stride = kVertexFloats * 4;
  desc.depth.test_enable = true;
  desc.depth.write_enable = state.depth_write;
  desc.depth.func = nrhi::CompareFunc::kLessEqual;
  desc.cull = state.cull == NativeDrawState::kFront  ? nrhi::CullMode::kFront
              : state.cull == NativeDrawState::kBack ? nrhi::CullMode::kBack
                                                     : nrhi::CullMode::kNone;
  if (state.blend == NativeDrawState::kAlpha) {
    desc.blend.enable = true;
    desc.blend.src = nrhi::BlendFactor::kSrcAlpha;
    desc.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  } else if (state.blend == NativeDrawState::kAdditive) {
    desc.blend.enable = true;
    desc.blend.src = nrhi::BlendFactor::kOne;
    desc.blend.dst = nrhi::BlendFactor::kOne;
  }
  desc.blend.src_alpha = nrhi::BlendFactor::kOne;
  desc.blend.dst_alpha = nrhi::BlendFactor::kZero;
  desc.blend.write_mask = 0x7;
  desc.rtv_format = color_format;
  desc.dsv_format = nrhi::Format::kD32_FLOAT;
  slot = g.device->CreateGraphicsPipeline(desc);
  return slot;
}

// The game's final composite and lens passes (post.hlsl): the world is drawn
// into g.scene, then graded, distorted and vignetted into the guest output
// with the constants of the game's own draws.
constexpr uint32_t kPostConstantFloats = 40;  // see post.hlsl cbuffer C

bool EnsurePost(nrhi::Format format, uint32_t width, uint32_t height) {
  if (!g.post_layout) {
    nrhi::BindingLayoutDesc layout;
    layout.params[0].kind = nrhi::BindingParamKind::kConstants;
    layout.params[0].shader_register = 0;
    layout.params[0].count = kPostConstantFloats;
    layout.params[0].visibility = nrhi::Visibility::kPixel;
    layout.params[1].kind = nrhi::BindingParamKind::kTextureTable;
    layout.params[1].shader_register = 0;
    layout.params[1].count = 3;
    layout.params[1].visibility = nrhi::Visibility::kPixel;
    layout.param_count = 2;
    layout.static_samplers[0].shader_register = 0;
    layout.static_samplers[0].filter = nrhi::Filter::kLinear;
    layout.static_samplers[0].address = nrhi::AddressMode::kClamp;
    layout.static_sampler_count = 1;
    g.post_layout = g.device->CreateBindingLayout(layout);
    namespace sh = skate2::native_shaders;
    nrhi::ShaderDesc vs;
    vs.stage = nrhi::ShaderStage::kVertex;
    vs.name = "post_vs";
    vs.hlsl_source = sh::kPostHlsl;
    vs.entry_point = "vs_main";
    vs.spirv = sh::kPostVs;
    vs.spirv_size_bytes = sizeof(sh::kPostVs);
    g.post_vs = g.device->CreateShader(vs);
    nrhi::ShaderDesc ps = vs;
    ps.stage = nrhi::ShaderStage::kPixel;
    ps.name = "post_ps";
    ps.entry_point = "ps_main";
    ps.spirv = sh::kPostPs;
    ps.spirv_size_bytes = sizeof(sh::kPostPs);
    g.post_ps = g.device->CreateShader(ps);
  }
  if (!g.post_layout || !g.post_vs || !g.post_ps) return false;
  if (g.post_pipeline && g.post_format != format) {
    g.device->DestroyDeferred(g.post_pipeline);
    g.post_pipeline = nullptr;
  }
  if (!g.post_pipeline) {
    nrhi::GraphicsPipelineDesc desc;
    desc.layout = g.post_layout;
    desc.vs = g.post_vs;
    desc.ps = g.post_ps;
    desc.depth.test_enable = false;
    desc.depth.write_enable = false;
    desc.cull = nrhi::CullMode::kNone;
    desc.blend.write_mask = 0xF;
    desc.rtv_format = format;
    desc.dsv_format = nrhi::Format::kUnknown;
    g.post_pipeline = g.device->CreateGraphicsPipeline(desc);
    g.post_format = format;
    if (!g.post_pipeline) return false;
  }
  if (g.scene && (g.scene_width != width || g.scene_height != height ||
                  g.scene_format != format)) {
    if (g.scene_view) g.device->DestroyDeferred(g.scene_view);
    g.device->DestroyDeferred(g.scene);
    g.scene = nullptr;
    g.scene_view = nullptr;
  }
  if (!g.scene) {
    nrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kPixelShaderResource;
    g.scene = g.device->CreateTexture(desc);
    if (!g.scene) return false;
    nrhi::TextureViewDesc view;
    g.scene_view = g.device->CreateTextureView(g.scene, view);
    g.scene_width = width;
    g.scene_height = height;
    g.scene_format = format;
  }
  return g.scene_view != nullptr;
}

// The lens pass's vignette texture (tf2 of the game's draw), decoded again
// only when its fetch constant changes; nullptr while unavailable.
nrhi::TextureView* VignetteView(nrhi::Cmd* cmd, uint8_t* base, const uint32_t words[6]) {
  if (g.vignette_tried && std::memcmp(g.vignette_fetch, words, sizeof(g.vignette_fetch)) == 0) {
    return g.vignette.view;
  }
  g.vignette_tried = true;
  std::memcpy(g.vignette_fetch, words, sizeof(g.vignette_fetch));
  NativeTexture t;
  if (!DecodeNativeTexture(g.device, cmd, base, words, t)) {
    REXLOG_WARN("Native world: lens vignette texture not decodable");
    return g.vignette.view;  // keep the previous one, if any
  }
  if (g.vignette.view) g.device->DestroyDeferred(g.vignette.view);
  if (g.vignette.texture) g.device->DestroyDeferred(g.vignette.texture);
  g.vignette = t;
  return g.vignette.view;
}

// The composite's colour grading table as a slice atlas (NativeLutAtlas),
// rebuilt when its fetch constant changes; nullptr while unavailable.
nrhi::TextureView* GradeView(nrhi::Cmd* cmd, uint8_t* base, const uint32_t words[6]) {
  if (g.lut_tried && std::memcmp(g.lut_fetch, words, sizeof(g.lut_fetch)) == 0) {
    return g.lut.view;
  }
  g.lut_tried = true;
  std::memcpy(g.lut_fetch, words, sizeof(g.lut_fetch));
  std::vector<uint8_t> rgba;
  uint32_t n = 0;
  if (!NativeLutAtlas(base, words, rgba, n)) {
    REXLOG_WARN("Native world: colour grading table not decodable");
    return g.lut.view;
  }
  nrhi::TextureDesc desc;
  desc.width = n * n;
  desc.height = n;
  desc.format = nrhi::Format::kR8G8B8A8_UNORM;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  nrhi::Texture* texture = g.device->CreateTexture(desc);
  if (!texture) return g.lut.view;
  const uint32_t row = (n * n * 4 + nrhi::kRowPitchAlignment - 1) & ~(nrhi::kRowPitchAlignment - 1);
  nrhi::BufferDesc up;
  up.size = uint64_t(row) * n;
  up.heap = nrhi::HeapKind::kUpload;
  up.bind_class = nrhi::BufferBindClass::kCopySrc;
  nrhi::Buffer* upload = g.device->CreateBuffer(up);
  if (!upload) {
    g.device->DestroyDeferred(texture);
    return g.lut.view;
  }
  uint8_t* px = static_cast<uint8_t*>(g.device->Map(upload));
  for (uint32_t y = 0; y < n; ++y) {
    std::memcpy(px + size_t(y) * row, rgba.data() + size_t(y) * n * n * 4, size_t(n) * n * 4);
  }
  g.device->Unmap(upload);
  cmd->CopyBufferToTexture(texture, 0, 0, upload, 0, row, n * n, n, 1);
  cmd->Barrier(texture, nrhi::ResourceState::kCopyDest, nrhi::ResourceState::kPixelShaderResource);
  g.device->DestroyDeferred(upload);
  nrhi::TextureViewDesc view;
  nrhi::TextureView* v = g.device->CreateTextureView(texture, view);
  if (!v) {
    g.device->DestroyDeferred(texture);
    return g.lut.view;
  }
  if (g.lut.view) g.device->DestroyDeferred(g.lut.view);
  if (g.lut.texture) g.device->DestroyDeferred(g.lut.texture);
  g.lut.texture = texture;
  g.lut.view = v;
  g.lut.width = n;
  return g.lut.view;
}

bool EnsureDepth(uint32_t width, uint32_t height) {
  if (g.depth && g.width == width && g.height == height) return true;
  if (g.depth) g.device->DestroyDeferred(g.depth);
  nrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = nrhi::Format::kD32_FLOAT;
  desc.usage = nrhi::kTextureUsageDepthStencil;
  desc.initial_state = nrhi::ResourceState::kDepthWrite;
  desc.clear_depth = 1.0f;
  g.depth = g.device->CreateTexture(desc);
  g.width = width;
  g.height = height;
  return g.depth != nullptr;
}

// Asset GUID -> renderengine::Texture, filled by the RegisterTexture hook
// below from boot on (materials name their textures by GUID).
constexpr uint64_t kGuidMask = 0x7FFFFFFFFFFFFFFFull;
std::mutex g_texture_mutex;
std::unordered_map<uint64_t, uint32_t> g_textures;

}  // namespace

uint32_t NativeWorldTextureForGuid(uint64_t guid) {
  std::lock_guard<std::mutex> lock(g_texture_mutex);
  auto it = g_textures.find(guid & kGuidMask);
  return it == g_textures.end() ? 0 : it->second;
}

REX_EXTERN(__imp__sub_82B3BA88);

// cProcessArenaAsset::RegisterTexture (Skate 3 sub_82C9A618): r4 = 64-bit
// asset GUID, r5 = renderengine::Texture.
extern "C" REX_FUNC(sub_82B3BA88) {
  if (ctx.r5.u32) {
    std::lock_guard<std::mutex> lock(g_texture_mutex);
    g_textures[ctx.r4.u64 & kGuidMask] = ctx.r5.u32;
  }
  __imp__sub_82B3BA88(ctx, base);
}

// MeshContext -> mesh and its draw records.
bool ReadEntry(uint8_t* base, uint32_t ctx, Entry& e) {
  if (!ctx) return false;
  e.mesh = ReadU32(base, ReadU32(base, ctx));
  const uint32_t records = ReadU32(base, ctx + kCtxDrawRecords);
  e.record_count = ReadU32(base, ctx + kCtxDrawCount);
  if (!e.mesh || !records || e.record_count == 0) return false;
  if (e.record_count > kMaxRecords) e.record_count = kMaxRecords;
  uint8_t raw[kMaxRecords * 16];
  if (!TryRead(base, records, raw, e.record_count * 16)) return false;
  for (uint32_t r = 0; r < e.record_count; ++r) {
    e.records[r] = {Be32(raw + r * 16), Be32(raw + r * 16 + 4), Be32(raw + r * 16 + 8),
                    Be32(raw + r * 16 + 12)};
  }
  return true;
}

void NativeWorldOnDrawList(uint8_t* base, uint32_t view, uint32_t first, uint32_t count) {
  g_base.store(base, std::memory_order_relaxed);
  if (!(REXCVAR_GET(skate2_native_world) || g_capture_forced.load(std::memory_order_relaxed)) ||
      count == 0) {
    return;
  }
  const uint32_t list = ReadU32(base, view + kViewDrawList);
  if (!list) return;
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  // Several views can render per frame; keep the one with the most entries.
  if (g_building.view != view) {
    if (!g_building.entries.empty() && count <= g_building.entries.size()) return;
    g_building.entries.clear();
    g_building.view = view;
    const uint32_t camera = ReadU32(base, view + kViewCamera);
    for (int i = 0; i < 16; ++i) {
      const uint32_t bits = ReadU32(base, camera + kCameraViewProj + i * 4);
      std::memcpy(&g_building.view_proj[i], &bits, 4);
    }
    SolveEye(g_building.view_proj, g_building.eye);
  }
  for (uint32_t k = 0; k < count; ++k) {
    Entry e;
    if (ReadEntry(base, ReadU32(base, list + (first + k) * 8 + 4), e)) {
      g_building.entries.push_back(e);
    }
  }
}

void NativeWorldOnRenderMesh(uint8_t* base, uint32_t mesh_context) {
  if (!(REXCVAR_GET(skate2_native_world) || g_capture_forced.load(std::memory_order_relaxed))) {
    return;
  }
  Entry e;
  if (!ReadEntry(base, mesh_context, e)) return;
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  // Shadow and main passes render the same mesh: keep it once.
  for (const Entry& other : g_building.entities) {
    if (other.mesh == e.mesh) return;
  }
  if (g_building.entities.size() < 256) g_building.entities.push_back(e);
}

void NativeWorldOnSwap() {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (!g_building.entries.empty()) {
    std::swap(g_published, g_building);
  }
  g_building.entries.clear();
  g_building.entities.clear();
  g_building.view = 0;
}

// Builds every pipeline variant once, as soon as the device and the output
// format are known (menus), so turning the native world on or meeting a new
// state combination in play never compiles a pipeline mid-frame.
void WarmPipelines(nrhi::Device* device, nrhi::Format format, uint32_t width,
                   uint32_t height) {
  static bool warmed = false;
  if (warmed || !device || format == nrhi::Format::kUnknown) return;
  g.device = device;
  if (!EnsureShaders()) return;
  NativeDrawState state;
  for (uint8_t cull = 0; cull < 3; ++cull) {
    for (uint8_t blend = 0; blend < 3; ++blend) {
      for (int depth_write = 0; depth_write < 2; ++depth_write) {
        state.cull = NativeDrawState::Cull(cull);
        state.blend = NativeDrawState::Blend(blend);
        state.depth_write = depth_write != 0;
        GetPipeline(format, state);
        if (EnsureEntityShaders()) GetEntityPipeline(format, state);
      }
    }
  }
  // Sun shadows and the post pass too: built on first use they hitch the
  // first native frames on the Deck.
  if (EnsureEntityShaders()) GetEntityShadowPipeline();
  EnsureShadow();
  EnsurePost(format, width, height);
  warmed = true;
  REXLOG_INFO("Native world: {} pipeline variants built", 2 * 3 * 3 * 2 + 2);
}

void NativeWorldPrepare(const rex::graphics::NativeGuestOutputRenderContext& context) {
  if (context.guest_output) {
    WarmPipelines(context.device, context.guest_output->format(),
                  context.guest_output->width(), context.guest_output->height());
  }
}

bool NativeWorldRender(const rex::graphics::NativeGuestOutputRenderContext& context,
                       bool force) {
  g_capture_forced.store(force, std::memory_order_relaxed);
  if (!(REXCVAR_GET(skate2_native_world) || force) || !context.cmd || !context.guest_output ||
      !context.device) {
    return false;
  }
  uint8_t* base = g_base.load(std::memory_order_relaxed);
  Frame frame;
  {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    if (g_published.entries.empty()) return false;
    frame = g_published;
  }
  if (!base) return false;
  g.device = context.device;
  nrhi::Texture* out = context.guest_output;
  const uint32_t width = out->width(), height = out->height();
  if (!EnsureShaders() || !EnsureDepth(width, height)) return false;
  nrhi::Cmd* cmd = context.cmd;
  if (!EnsureWhite(cmd)) return false;

  // Phase 1 (outside the render pass): build missing meshes and decode their
  // textures, both bounded per frame; uploads are recorded as copies. Each
  // draw record takes the render state of the game's matching emulated draw
  // (opaque with back-face culling when unknown).
  struct Item {
    const MeshGpu* mesh;
    const DrawRecord* record;
    nrhi::TextureView* diffuse;
    nrhi::TextureView* lightmap;
    nrhi::TextureView* chroma;
    nrhi::TextureView* material[5];  // normal, detail, macro, specular, environment
    NativeDrawState state;
    bool matched;
  };
  std::vector<Item> opaque, blended_items;
  opaque.reserve(frame.entries.size() * 2);
  uint32_t builds = 0, texture_budget = kMaxTextureDecodesPerFrame;
  uint32_t matched = 0, alpha_tested = 0, blended = 0;
  for (const Entry& e : frame.entries) {
    auto it = g.meshes.find(e.mesh);
    if (it == g.meshes.end()) {
      if (builds >= kMaxBuildsPerFrame) continue;
      ++builds;
      it = g.meshes.emplace(e.mesh, BuildMesh(base, e.mesh)).first;
      if (it->second.failed) ++g.failed_meshes;
    }
    const MeshGpu& m = it->second;
    if (m.failed) continue;
    nrhi::TextureView* diffuse = TextureView(cmd, base, m.diffuse, texture_budget);
    nrhi::TextureView* lightmap = TextureView(cmd, base, m.lightmap, texture_budget);
    nrhi::TextureView* chroma = TextureView(cmd, base, m.chroma, texture_budget);
    nrhi::TextureView* material[5] = {TextureView(cmd, base, m.normal, texture_budget),
                                      TextureView(cmd, base, m.detail, texture_budget),
                                      TextureView(cmd, base, m.macro, texture_budget),
                                      TextureView(cmd, base, m.specular, texture_budget),
                                      EnvironmentView(cmd, base, m.environment, texture_budget)};
    for (uint32_t r = 0; r < e.record_count; ++r) {
      const DrawRecord& rec = e.records[r];
      if (rec.index_count == 0 || rec.start_index + rec.index_count > m.index_count) continue;
      if (rec.prim != 4 && rec.prim != 6) continue;
      Item item{&m, &rec, diffuse, lightmap, chroma,
                {material[0], material[1], material[2], material[3], material[4]}, {},
                false};
      item.matched =
          NativeDrawStateLookup(m.vb_phys, m.ib_phys + rec.start_index * 2, item.state);
      matched += item.matched;
      alpha_tested += item.state.alpha_ref > 0.0f;
      if (item.state.blend != NativeDrawState::kOpaque) {
        ++blended;
        blended_items.push_back(item);
      } else {
        opaque.push_back(item);
      }
    }
  }

  // Entities: each draw record takes the vertex shader constants of the
  // game's matching emulated draw (clip matrix, bone palette, fog), copied
  // into this frame's palette block. A draw seen for the first time is asked
  // for and appears from the next frame on.
  struct EntityItem {
    const MeshGpu* mesh;
    const DrawRecord* record;
    nrhi::TextureView* diffuse;
    NativeDrawState state;
    float constants[kEntityConstantFloats];
    uint32_t palette_offset;
  };
  std::vector<EntityItem> entity_items;
  if (!frame.entities.empty() && EnsureEntityShaders()) {
    const uint32_t block = g.palette_frame % kPaletteBlocks;
    // Reuse the block only once the GPU is done with the frame that used it.
    const bool block_free =
        g.device->CompletedSubmission() >= g.palette_block_submission[block];
    uint32_t slot = 0;
    const NativeFrameConstants& world_fc = NativeDrawFrameConstants();
    for (const Entry& e : frame.entities) {
      if (!block_free) break;
      auto it = g.entity_meshes.find(e.mesh);
      if (it == g.entity_meshes.end()) {
        if (builds >= kMaxBuildsPerFrame) continue;
        ++builds;
        it = g.entity_meshes.emplace(e.mesh, BuildEntityMesh(base, e.mesh)).first;
        if (it->second.failed) ++g.failed_meshes;
      }
      const MeshGpu& m = it->second;
      if (m.failed) continue;
      nrhi::TextureView* diffuse = TextureView(cmd, base, m.diffuse, texture_budget);
      for (uint32_t r = 0; r < e.record_count && slot < kPaletteSlotsPerBlock; ++r) {
        const DrawRecord& rec = e.records[r];
        if (rec.index_count == 0 || rec.start_index + rec.index_count > m.index_count) continue;
        if (rec.prim != 4 && rec.prim != 6) continue;
        const uint32_t index_base = m.ib_phys + rec.start_index * 2;
        NativeDrawStateWantConstants(m.vb_phys, index_base);
        NativeDrawConstants dc;
        if (!NativeDrawStateConstants(m.vb_phys, index_base, dc)) continue;
        const float* vs = dc.vs;
        EntityItem item{&m, &rec, diffuse, {}, {}, 0};
        NativeDrawStateLookup(m.vb_phys, index_base, item.state);
        std::memcpy(item.constants, vs, 16 * sizeof(float));  // c0..c3 clip rows
        // Slot: the bone palette, then the character lighting (entity.hlsl).
        item.palette_offset = (block * kPaletteSlotsPerBlock + slot) * kPaletteSlotBytes;
        float* slot_rows = reinterpret_cast<float*>(g.palette_map + item.palette_offset);
        ++slot;
        if (m.skinned) {
          std::memcpy(item.constants + 16, vs + 4 * 4, 12 * sizeof(float));  // c4..c6
          const uint32_t first = PaletteBase(dc.vertex_shader);
          const uint32_t rows = std::min<uint32_t>(kPaletteBoneRows, 256 - first);
          std::memcpy(slot_rows, vs + first * 4, rows * 4 * sizeof(float));
        }  // rigid meshes: no world position, no fog
        const bool lit = WriteCharacterLighting(dc, slot_rows + kPaletteBoneRows * 4);
        item.constants[28] = m.skinned ? 1.0f : 0.0f;
        item.constants[29] = lit ? slot_rows[(kPaletteBoneRows + 9) * 4 + 3]
                                 : (world_fc.valid ? world_fc.exposure : 2.0f);
        item.constants[35] = lit ? 1.0f : 0.0f;
        item.constants[30] = item.state.alpha_ref;
        item.constants[31] = diffuse ? 1.0f : 0.0f;
        // Sun direction of the game's world shaders (PS c4, Freeskate daytime).
        item.constants[32] = 0.05036f;
        item.constants[33] = 0.5552f;
        item.constants[34] = 0.8302f;
        entity_items.push_back(item);
      }
    }
    if (block_free) {
      g.palette_block_submission[block] = g.device->CurrentSubmission();
      ++g.palette_frame;
    }
  }

  // Sun shadow map from the skinned entities, before the world pass.
  bool shadow_drawn = false;
  const NativeFrameConstants& sfc = NativeDrawFrameConstants();
  if (sfc.shadow_valid && !entity_items.empty() && EnsureShadow() && GetEntityShadowPipeline()) {
    cmd->Barrier(g.shadow, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kDepthWrite);
    cmd->FlushBarriers();
    cmd->SetRenderTargets(nullptr, g.shadow);
    cmd->ClearDepth(g.shadow, 1.0f);
    cmd->SetBindingLayout(g.entity_layout);
    cmd->SetPipeline(g.entity_shadow_pipeline);
    const float* c = sfc.shadow;
    for (int cascade = 0; cascade < 2; ++cascade) {
      nrhi::Viewport vp;
      vp.x = float(cascade * kShadowWidth / 2);
      vp.width = float(kShadowWidth / 2);
      vp.height = float(kShadowHeight);
      cmd->SetViewport(vp);
      nrhi::Rect rect;
      rect.left = int32_t(cascade * kShadowWidth / 2);
      rect.right = rect.left + int32_t(kShadowWidth / 2);
      rect.bottom = int32_t(kShadowHeight);
      cmd->SetScissor(rect);
      // Light clip rows: X, Y (scaled for the second cascade), stored depth, 1.
      const float sx = cascade ? c[4] : 1.0f, ox = cascade ? c[6] : 0.0f;
      const float sy = cascade ? c[5] : 1.0f, oy = cascade ? c[7] : 0.0f;
      float rows[16] = {c[0] * sx, c[1] * sx, c[2] * sx, c[3] * sx + ox,
                        c[8] * sy, c[9] * sy, c[10] * sy, c[11] * sy + oy,
                        c[12] * 0.05f, c[13] * 0.05f, c[14] * 0.05f, c[15] * 0.05f + 0.5f,
                        0.0f, 0.0f, 0.0f, 1.0f};
      const MeshGpu* shadow_mesh = nullptr;
      for (const EntityItem& d : entity_items) {
        if (!d.mesh->skinned) continue;  // rigid entities carry no world position
        float constants_shadow[kEntityConstantFloats];
        std::memcpy(constants_shadow, d.constants, sizeof(constants_shadow));
        std::memcpy(constants_shadow, rows, sizeof(rows));
        cmd->SetRootConstants(0, kEntityConstantFloats, constants_shadow, 0);
        cmd->SetConstantBuffer(1, g.palette, d.palette_offset);
        cmd->SetTexture(2, d.diffuse ? d.diffuse : g.white_view);
        if (d.mesh != shadow_mesh) {
          cmd->SetVertexBuffer(d.mesh->vb, 0, d.mesh->vb_bytes, kEntityVertexBytes);
          cmd->SetIndexBuffer(d.mesh->ib, 0, d.mesh->ib_bytes);
          shadow_mesh = d.mesh;
        }
        cmd->SetPrimitiveTopology(d.record->prim == 4 ? nrhi::PrimitiveTopology::kTriangleList
                                                      : nrhi::PrimitiveTopology::kTriangleStrip);
        cmd->DrawIndexed(d.record->index_count, d.record->start_index,
                         int32_t(d.record->base_vertex));
      }
    }
    cmd->Barrier(g.shadow, nrhi::ResourceState::kDepthWrite,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
    shadow_drawn = true;
  }

  // The game's lens pass, when its draw was seen: the world goes to g.scene.
  nrhi::TextureView* vignette = nullptr;
  if (REXCVAR_GET(skate2_native_lens) && sfc.lens_valid &&
      EnsurePost(out->format(), width, height)) {
    vignette = VignetteView(cmd, base, sfc.lens_vignette);
  }
  nrhi::TextureView* lut =
      vignette && sfc.grade_valid ? GradeView(cmd, base, sfc.grade_lut) : nullptr;
  nrhi::Texture* target = vignette ? g.scene : out;

  // Phase 2: the world pass, opaque (and alpha-tested) first, then blended in
  // draw-list order.
  if (vignette) {
    cmd->Barrier(g.scene, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
  } else {
    cmd->Barrier(out, nrhi::ResourceState::kGuestOutput, nrhi::ResourceState::kRenderTarget);
  }
  cmd->FlushBarriers();
  cmd->SetRenderTargets(target, g.depth);
  const float sky[4] = {0.52f, 0.66f, 0.82f, 1.0f};
  cmd->ClearRenderTarget(target, sky);
  cmd->ClearDepth(g.depth, 1.0f);
  nrhi::Viewport viewport;
  viewport.width = float(width);
  viewport.height = float(height);
  cmd->SetViewport(viewport);
  nrhi::Rect scissor;
  scissor.right = int32_t(width);
  scissor.bottom = int32_t(height);
  cmd->SetScissor(scissor);
  cmd->SetBindingLayout(g.layout);

  float constants[kConstantFloats] = {};
  std::memcpy(constants, frame.view_proj, sizeof(frame.view_proj));
  // Fog (VS c5/c6) and exposure (PS c6.x) of the game's main world shader,
  // taken from its emulated draws; the Freeskate daytime values until seen.
  const NativeFrameConstants& fc = NativeDrawFrameConstants();
  const float default_fog[4] = {0.004f, -0.2f, 1.4f, 0.0f};
  const float default_fog_color[4] = {0.06731f, 0.1337f, 0.2692f, 0.0f};
  constants[17] = float(REXCVAR_GET(skate2_native_lm_mode));
  constants[18] = fc.valid ? fc.exposure : 2.0f;
  constants[20] = frame.eye[0];
  constants[21] = frame.eye[1];
  constants[22] = frame.eye[2];
  std::memcpy(constants + 24, fc.valid ? fc.fog : default_fog, 4 * sizeof(float));
  std::memcpy(constants + 28, fc.valid ? fc.fog_color : default_fog_color, 4 * sizeof(float));
  // Sun direction of the main world material (PS c4); Freeskate daytime until seen.
  const float default_sun[4] = {0.05036f, 0.5552f, 0.8302f, 0.0f};
  std::memcpy(constants + 40, fc.sun_valid ? fc.sun : default_sun, 4 * sizeof(float));
  if (shadow_drawn) {
    std::memcpy(constants + 44, sfc.shadow, 16 * sizeof(float));
    constants[60] = 1.0f;
  }

  uint32_t drawn = 0, textured = 0;
  nrhi::Pipeline* bound_pipeline = nullptr;
  const MeshGpu* bound_mesh = nullptr;
  auto draw = [&](const Item& d) {
    nrhi::Pipeline* pipeline = GetPipeline(out->format(), d.state);
    if (!pipeline) return;
    if (pipeline != bound_pipeline) {
      cmd->SetPipeline(pipeline);
      bound_pipeline = pipeline;
    }
    const MeshGpu& m = *d.mesh;
    constants[16] = d.diffuse ? 1.0f : 0.0f;
    constants[19] = float(m.lightmap_channel);
    constants[32] = d.state.alpha_ref;
    constants[33] = d.state.depth_offset ? 1.0f : 0.0f;
    constants[34] = d.matched ? 1.0f : 0.0f;
    // The main world material path needs its four material textures.
    const bool material_path = d.state.macro_scale > 0.0f && d.material[0] && d.material[1] &&
                               d.material[2] && d.material[3] && d.diffuse && d.lightmap &&
                               d.chroma;
    constants[36] = d.state.macro_scale;
    constants[37] = d.state.detail_scale;
    constants[38] = material_path ? 1.0f : 0.0f;
    constants[39] = d.material[4] ? 1.0f : 0.0f;
    cmd->SetRootConstants(0, kConstantFloats, constants, 0);
    nrhi::TextureView* views[8] = {d.diffuse ? d.diffuse : g.white_view,
                                   d.lightmap ? d.lightmap : g.white_view,
                                   d.chroma ? d.chroma : g.white_view,
                                   d.material[0] ? d.material[0] : g.white_view,
                                   d.material[1] ? d.material[1] : g.white_view,
                                   d.material[2] ? d.material[2] : g.white_view,
                                   d.material[3] ? d.material[3] : g.white_view,
                                   d.material[4] ? d.material[4] : g.white_view};
    cmd->SetTextures(1, views, 8);
    cmd->SetTexture(2, shadow_drawn ? g.shadow_view : g.white_view);
    if (&m != bound_mesh) {
      cmd->SetVertexBuffer(m.vb, 0, m.vb_bytes, kVertexFloats * 4);
      cmd->SetIndexBuffer(m.ib, 0, m.ib_bytes);
      bound_mesh = &m;
    }
    cmd->SetPrimitiveTopology(d.record->prim == 4 ? nrhi::PrimitiveTopology::kTriangleList
                                                  : nrhi::PrimitiveTopology::kTriangleStrip);
    cmd->DrawIndexed(d.record->index_count, d.record->start_index,
                     int32_t(d.record->base_vertex));
    ++drawn;
    textured += d.diffuse ? 1 : 0;
  };
  for (const Item& d : opaque) draw(d);
  uint32_t entity_drawn = 0;
  auto draw_entities = [&](bool blended_pass) {
    bool layout_set = false;
    nrhi::Pipeline* entity_pipeline = nullptr;
    const MeshGpu* entity_mesh = nullptr;
    for (const EntityItem& d : entity_items) {
      if ((d.state.blend != NativeDrawState::kOpaque) != blended_pass) continue;
      nrhi::Pipeline* pipeline = GetEntityPipeline(out->format(), d.state);
      if (!pipeline) continue;
      if (!layout_set) {
        cmd->SetBindingLayout(g.entity_layout);
        layout_set = true;
      }
      if (pipeline != entity_pipeline) {
        cmd->SetPipeline(pipeline);
        entity_pipeline = pipeline;
      }
      cmd->SetRootConstants(0, kEntityConstantFloats, d.constants, 0);
      cmd->SetConstantBuffer(1, g.palette, d.palette_offset);
      cmd->SetTexture(2, d.diffuse ? d.diffuse : g.white_view);
      if (d.mesh != entity_mesh) {
        cmd->SetVertexBuffer(d.mesh->vb, 0, d.mesh->vb_bytes, kEntityVertexBytes);
        cmd->SetIndexBuffer(d.mesh->ib, 0, d.mesh->ib_bytes);
        entity_mesh = d.mesh;
      }
      cmd->SetPrimitiveTopology(d.record->prim == 4 ? nrhi::PrimitiveTopology::kTriangleList
                                                    : nrhi::PrimitiveTopology::kTriangleStrip);
      cmd->DrawIndexed(d.record->index_count, d.record->start_index,
                       int32_t(d.record->base_vertex));
      ++entity_drawn;
    }
  };
  draw_entities(false);
  cmd->SetBindingLayout(g.layout);
  bound_pipeline = nullptr;
  bound_mesh = nullptr;
  for (const Item& d : blended_items) draw(d);
  draw_entities(true);
  if (vignette) {
    cmd->Barrier(g.scene, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->Barrier(out, nrhi::ResourceState::kGuestOutput, nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
    cmd->SetRenderTargets(out, nullptr);
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    cmd->SetBindingLayout(g.post_layout);
    cmd->SetPipeline(g.post_pipeline);
    float post[kPostConstantFloats] = {};
    std::memcpy(post, sfc.lens, sizeof(sfc.lens));
    if (sfc.grade_valid) {
      std::memcpy(post + 16, sfc.grade, sizeof(sfc.grade));
    } else {
      post[28] = 1.0f;  // scene scale 1, no table
    }
    post[36] = lut ? 1.0f : 0.0f;
    post[37] = float(g.lut.width);
    cmd->SetRootConstants(0, kPostConstantFloats, post, 0);
    nrhi::TextureView* post_views[3] = {g.scene_view, vignette, lut ? lut : g.white_view};
    cmd->SetTextures(1, post_views, 3);
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    cmd->Draw(3, 0);
  }
  cmd->Barrier(out, nrhi::ResourceState::kRenderTarget, nrhi::ResourceState::kGuestOutput);
  cmd->FlushBarriers();
  if (++g.frames % 300 == 1 || g.frames == 90) {
    REXLOG_INFO("Native world: {} entries, {} draws ({} textured, {} with game state, {} "
                "alpha-tested, {} blended), {} meshes ({} failed), {} textures ({} failed, {} "
                "re-decoded), "
                "{} built now; {} entities, {} entity draws",
                frame.entries.size(), drawn, textured, matched, alpha_tested, blended,
                g.meshes.size(), g.failed_meshes, g.textures.size(), g.failed_textures,
                g.redecoded_textures, builds,
                frame.entities.size(), entity_drawn);
  }
  return true;
}
