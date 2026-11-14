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

#include "native/native_shaders_spirv.h"

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/graphics/native_rhi.h>
#include <rex/logging.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

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
constexpr uint32_t kMeshVertexDescriptor = 0x28;
constexpr uint32_t kMeshIndexBuffer = 0x30;
constexpr uint32_t kMeshVertexBuffer = 0x34;
constexpr uint32_t kBufferAddress = 0x18;   // +0x18 address, +0x20 size

constexpr uint32_t kMaxRecords = 8;
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
  std::vector<Entry> entries;
};

std::mutex g_frame_mutex;
Frame g_building;
Frame g_published;
std::atomic<uint8_t*> g_base{nullptr};
// Set by the renderer when the world flag file forces native rendering.
std::atomic<bool> g_capture_forced{false};

// ---- guarded guest memory access -------------------------------------------

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

struct MeshGpu {
  nrhi::Buffer* vb = nullptr;
  nrhi::Buffer* ib = nullptr;
  uint32_t vb_bytes = 0;
  uint32_t ib_bytes = 0;
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  bool failed = false;
};

struct Gpu {
  nrhi::Device* device = nullptr;
  nrhi::BindingLayout* layout = nullptr;
  nrhi::Shader* vs = nullptr;
  nrhi::Shader* ps = nullptr;
  nrhi::Pipeline* pipeline = nullptr;
  nrhi::Format color_format = nrhi::Format::kUnknown;
  nrhi::Texture* depth = nullptr;
  uint32_t width = 0, height = 0;
  std::unordered_map<uint32_t, MeshGpu> meshes;
  uint64_t frames = 0;
  uint32_t failed_meshes = 0;
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

// Decode a guest mesh (vertex descriptor, positions, indices) into host
// buffers. Vertex descriptor: element count u16 at +8, 16-byte elements from
// +0x10 (stream u16 @0, offset u16 @2, type u32 @4 with the Xenos format in
// the low 6 bits, usage u8 @9), the stride byte after the last element.
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
  int pos_offset = -1;
  uint32_t pos_format = 0;
  for (uint32_t i = 0; i < elements; ++i) {
    const uint8_t* e = table + i * 16;
    const uint32_t stream = uint32_t(e[0]) << 8 | e[1];
    if (stream == 0 && e[9] == 0) {  // usage 0 = position
      pos_offset = int(uint32_t(e[2]) << 8 | e[3]);
      pos_format = Be32(e + 4) & 0x3F;
      break;
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
  std::vector<float> positions(size_t(vertex_count) * 3);
  for (uint32_t v = 0; v < vertex_count; ++v) {
    const uint8_t* p = raw.data() + size_t(v) * stride + pos_offset;
    for (int c = 0; c < 3; ++c) {
      if (pos_format == 57) {
        const uint32_t bits = Be32(p + c * 4);
        std::memcpy(&positions[size_t(v) * 3 + c], &bits, 4);
      } else {
        positions[size_t(v) * 3 + c] = HalfToFloat(uint16_t(p[c * 2] << 8 | p[c * 2 + 1]));
      }
    }
  }
  std::vector<uint16_t> indices(ib_count);
  if (!TryRead(base, ib_addr, indices.data(), ib_count * 2)) return out;
  for (uint16_t& i : indices) i = uint16_t(i >> 8 | i << 8);

  out.vb = UploadBuffer(g.device, positions.data(), uint32_t(positions.size() * 4));
  out.ib = UploadBuffer(g.device, indices.data(), ib_count * 2);
  if (!out.vb || !out.ib) return out;
  out.vb_bytes = uint32_t(positions.size() * 4);
  out.ib_bytes = ib_count * 2;
  out.vertex_count = vertex_count;
  out.index_count = ib_count;
  out.failed = false;
  return out;
}

bool EnsurePipeline(nrhi::Format color_format) {
  if (g.pipeline && g.color_format == color_format) return true;
  if (!g.layout) {
    nrhi::BindingLayoutDesc layout;
    layout.params[0].kind = nrhi::BindingParamKind::kConstants;
    layout.params[0].shader_register = 0;
    layout.params[0].count = 20;  // float4x4 view_proj + float4 color
    layout.param_count = 1;
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
  if (!g.layout || !g.vs || !g.ps) return false;
  nrhi::InputElementDesc position;
  position.semantic_name = "POSITION";
  position.location = 0;
  position.format = nrhi::Format::kR32G32B32_FLOAT;
  nrhi::GraphicsPipelineDesc desc;
  desc.layout = g.layout;
  desc.vs = g.vs;
  desc.ps = g.ps;
  desc.input_elements = &position;
  desc.input_element_count = 1;
  desc.vertex_stride = 12;
  desc.depth.test_enable = true;
  desc.depth.write_enable = true;
  desc.depth.func = nrhi::CompareFunc::kLessEqual;
  desc.cull = nrhi::CullMode::kNone;
  desc.rtv_format = color_format;
  desc.dsv_format = nrhi::Format::kD32_FLOAT;
  if (g.pipeline) g.device->DestroyDeferred(g.pipeline);
  g.pipeline = g.device->CreateGraphicsPipeline(desc);
  g.color_format = color_format;
  return g.pipeline != nullptr;
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

}  // namespace

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
  if (!EnsurePipeline(out->format()) || !EnsureDepth(width, height)) return false;

  nrhi::Cmd* cmd = context.cmd;
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
  cmd->SetPipeline(g.pipeline);

  float constants[20];
  std::memcpy(constants, frame.view_proj, sizeof(frame.view_proj));
  uint32_t builds = 0, drawn = 0;
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
    // A stable per-mesh tint so neighbouring pieces read apart.
    const uint32_t h = e.mesh * 2654435761u;
    constants[16] = 0.55f + float((h >> 8) & 0x3F) / 255.0f;
    constants[17] = 0.55f + float((h >> 16) & 0x3F) / 255.0f;
    constants[18] = 0.55f + float((h >> 24) & 0x3F) / 255.0f;
    constants[19] = 1.0f;
    cmd->SetRootConstants(0, 20, constants, 0);
    cmd->SetVertexBuffer(m.vb, 0, m.vb_bytes, 12);
    cmd->SetIndexBuffer(m.ib, 0, m.ib_bytes);
    for (uint32_t r = 0; r < e.record_count; ++r) {
      const DrawRecord& d = e.records[r];
      if (d.index_count == 0 || d.start_index + d.index_count > m.index_count) continue;
      if (d.prim == 4) {
        cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      } else if (d.prim == 6) {
        cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
      } else {
        continue;
      }
      cmd->DrawIndexed(d.index_count, d.start_index, int32_t(d.base_vertex));
      ++drawn;
    }
  }
  cmd->Barrier(out, nrhi::ResourceState::kRenderTarget, nrhi::ResourceState::kGuestOutput);
  cmd->FlushBarriers();
  if (++g.frames % 300 == 1) {
    REXLOG_INFO("Native world: {} entries, {} draws, {} meshes cached ({} failed), {} built now",
                frame.entries.size(), drawn, g.meshes.size(), g.failed_meshes, builds);
  }
  return true;
}
