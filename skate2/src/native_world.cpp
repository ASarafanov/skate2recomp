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
                     "2 lightmap intensity, 3 chromaticity, 4 game state found (green) or not (red)")
    .range(0, 4)
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
constexpr uint32_t kConstantFloats = 36;  // world.hlsl cbuffer C
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

constexpr uint32_t kVertexFloats = 7;  // position, texcoord 0, texcoord 1 (lightmap)
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
  uint32_t lightmap_channel = 1;  // which lightmap channel holds this mesh's light
  // Guest physical addresses, the keys of the emulated draw state.
  uint32_t vb_phys = 0;
  uint32_t ib_phys = 0;
  bool failed = false;
};

struct TextureGpu {
  NativeTexture tex;
  bool failed = false;
};

struct Gpu {
  nrhi::Device* device = nullptr;
  nrhi::BindingLayout* layout = nullptr;
  nrhi::Shader* vs = nullptr;
  nrhi::Shader* ps = nullptr;
  // [cull][blend][depth write], built on first use (NativeDrawState enums).
  nrhi::Pipeline* pipelines[3][3][2] = {};
  nrhi::Format color_format = nrhi::Format::kUnknown;
  nrhi::Texture* depth = nullptr;
  uint32_t width = 0, height = 0;
  nrhi::Texture* white = nullptr;
  nrhi::TextureView* white_view = nullptr;
  std::unordered_map<uint32_t, MeshGpu> meshes;
  std::unordered_map<uint32_t, TextureGpu> textures;
  uint64_t frames = 0;
  uint32_t failed_meshes = 0;
  uint32_t failed_textures = 0;
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
    } else if (std::memcmp(name, "chromaticity", 11) == 0) {
      slot = &out.chroma;
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
  int pos_offset = -1, uv_offset = -1, uv2_offset = -1;
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
    }
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

// Returns the view of a decoded guest texture, decoding it on first use
// (bounded per frame by `budget`); nullptr while unavailable.
nrhi::TextureView* TextureView(nrhi::Cmd* cmd, uint8_t* base, uint32_t texture,
                               uint32_t& budget) {
  if (!texture) return nullptr;
  auto it = g.textures.find(texture);
  if (it == g.textures.end()) {
    if (budget == 0) return nullptr;
    --budget;
    TextureGpu t;
    t.failed = !DecodeNativeTextureObject(g.device, cmd, base, texture, t.tex);
    if (t.failed) ++g.failed_textures;
    it = g.textures.emplace(texture, t).first;
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
    layout.params[1].shader_register = 0;  // t0 diffuse, t1 lightmap, t2 chromaticity
    layout.params[1].count = 3;
    layout.params[1].visibility = nrhi::Visibility::kPixel;
    layout.param_count = 2;
    layout.static_samplers[0].shader_register = 0;
    layout.static_samplers[0].filter = nrhi::Filter::kLinear;
    layout.static_samplers[0].address = nrhi::AddressMode::kWrap;
    layout.static_samplers[0].max_anisotropy = 8;
    layout.static_samplers[1].shader_register = 1;
    layout.static_samplers[1].filter = nrhi::Filter::kLinear;
    layout.static_samplers[1].address = nrhi::AddressMode::kClamp;
    layout.static_sampler_count = 2;
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
    for (auto& a : g.pipelines)
      for (auto& b : a)
        for (nrhi::Pipeline*& p : b) {
          if (p) g.device->DestroyDeferred(p);
          p = nullptr;
        }
    g.color_format = color_format;
  }
  nrhi::Pipeline*& slot = g.pipelines[state.cull][state.blend][state.depth_write ? 1 : 0];
  if (slot) return slot;
  nrhi::InputElementDesc inputs[3];
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
  nrhi::GraphicsPipelineDesc desc;
  desc.layout = g.layout;
  desc.vs = g.vs;
  desc.ps = g.ps;
  desc.input_elements = inputs;
  desc.input_element_count = 3;
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
    const uint32_t ctx = ReadU32(base, list + (first + k) * 8 + 4);
    if (!ctx) continue;
    Entry e;
    e.mesh = ReadU32(base, ReadU32(base, ctx));
    const uint32_t records = ReadU32(base, ctx + kCtxDrawRecords);
    e.record_count = ReadU32(base, ctx + kCtxDrawCount);
    if (!e.mesh || !records || e.record_count == 0) continue;
    if (e.record_count > kMaxRecords) e.record_count = kMaxRecords;
    uint8_t raw[kMaxRecords * 16];
    if (!TryRead(base, records, raw, e.record_count * 16)) continue;
    for (uint32_t r = 0; r < e.record_count; ++r) {
      e.records[r] = {Be32(raw + r * 16), Be32(raw + r * 16 + 4), Be32(raw + r * 16 + 8),
                      Be32(raw + r * 16 + 12)};
    }
    g_building.entries.push_back(e);
  }
}

void NativeWorldOnSwap() {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (!g_building.entries.empty()) {
    std::swap(g_published, g_building);
  }
  g_building.entries.clear();
  g_building.view = 0;
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
    for (uint32_t r = 0; r < e.record_count; ++r) {
      const DrawRecord& rec = e.records[r];
      if (rec.index_count == 0 || rec.start_index + rec.index_count > m.index_count) continue;
      if (rec.prim != 4 && rec.prim != 6) continue;
      Item item{&m, &rec, diffuse, lightmap, chroma, {}, false};
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

  // Phase 2: the world pass, opaque (and alpha-tested) first, then blended in
  // draw-list order.
  cmd->Barrier(out, nrhi::ResourceState::kGuestOutput, nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(out, g.depth);
  const float sky[4] = {0.52f, 0.66f, 0.82f, 1.0f};
  cmd->ClearRenderTarget(out, sky);
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
    cmd->SetRootConstants(0, kConstantFloats, constants, 0);
    nrhi::TextureView* views[3] = {d.diffuse ? d.diffuse : g.white_view,
                                   d.lightmap ? d.lightmap : g.white_view,
                                   d.chroma ? d.chroma : g.white_view};
    cmd->SetTextures(1, views, 3);
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
  for (const Item& d : blended_items) draw(d);
  cmd->Barrier(out, nrhi::ResourceState::kRenderTarget, nrhi::ResourceState::kGuestOutput);
  cmd->FlushBarriers();
  if (++g.frames % 300 == 1 || g.frames == 90) {
    REXLOG_INFO("Native world: {} entries, {} draws ({} textured, {} with game state, {} "
                "alpha-tested, {} blended), {} meshes ({} failed), {} textures ({} failed), "
                "{} built now",
                frame.entries.size(), drawn, textured, matched, alpha_tested, blended,
                g.meshes.size(), g.failed_meshes, g.textures.size(), g.failed_textures, builds);
  }
  return true;
}
