// Diagnostics for porting the native renderer: log what the game's render
// functions receive, to confirm the Skate 2 structure layouts against the
// Skate 3 ones the renderer was written for. Enabled with
// --skate2_native_probe=true; otherwise every hook only calls the original.
//
// Hooked (Skate 2 address = Skate 3 counterpart):
//   sub_823A7420 = D3D Swap (sub_82B82E08): frame boundary, per-frame counts
//   sub_82365850 = RenderMesh (sub_82795AD8): r4 MeshContext, r5 VertexProgramState
//   sub_82364648 = SceneRenderView draw list (sub_827FAF50): r3 view, r4 first, r5 count

#include "native_world.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(skate2_native_probe, false, "Skate2",
                    "Debug: log render-function arguments and structures (native renderer port)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_823A7420);
REX_EXTERN(__imp__sub_82365850);
REX_EXTERN(__imp__sub_82364648);
REX_EXTERN(__imp__sub_823642E8);

namespace {

constexpr uint64_t kDumpEvery = 600;  // frames between detailed dumps

std::atomic<uint64_t> g_frame{0};
std::atomic<uint32_t> g_render_mesh{0};
std::atomic<uint32_t> g_draw_lists{0};
std::atomic<uint32_t> g_draw_list_entries{0};
std::atomic<uint32_t> g_dumped_mesh{0};
std::atomic<uint32_t> g_dumped_list{0};
std::atomic<uint32_t> g_vs_bank{0};

bool Enabled() { return REXCVAR_GET(skate2_native_probe); }

// World-scene detection, always on (cheap): frames whose scene draw list
// carried entries. Menus and loading screens draw no world.
std::atomic<uint32_t> g_world_entries{0};
std::atomic<uint32_t> g_last_world_entries{0};

bool DumpFrame() { return g_frame.load(std::memory_order_relaxed) % kDumpEvery == 0; }

// Guest physical addresses (0xE0000000+) live 4 KiB higher in the host mapping.
uint8_t* Host(uint8_t* base, uint32_t addr) {
  return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
}

// Byte-wise copy under SEH so a stale pointer cannot crash the game.
bool TryRead(uint8_t* base, uint32_t addr, uint8_t* out, uint32_t size) {
  if (addr < 0x10000 || uint64_t(addr) + size > 0x100000000ull) return false;
  const volatile uint8_t* src = Host(base, addr);
  __try {
    for (uint32_t i = 0; i < size; ++i) out[i] = src[i];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

uint32_t ReadU32(uint8_t* base, uint32_t addr, bool* ok = nullptr) {
  uint8_t b[4];
  const bool good = TryRead(base, addr, b, 4);
  if (ok) *ok = good;
  return good ? (uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]) : 0;
}

std::string Hex(uint8_t* base, uint32_t addr, uint32_t size) {
  uint8_t buf[256];
  if (size > sizeof(buf) || !TryRead(base, addr, buf, size)) return "<unreadable>";
  std::string s;
  char tmp[12];
  for (uint32_t i = 0; i < size; i += 4) {
    std::snprintf(tmp, sizeof(tmp), "%02X%02X%02X%02X ", buf[i], buf[i + 1], buf[i + 2],
                  buf[i + 3]);
    if (i % 32 == 0) {
      char off[8];
      std::snprintf(off, sizeof(off), "\n  +%02X ", i);
      s += off;
    }
    s += tmp;
  }
  return s;
}

}  // namespace

std::string Floats(uint8_t* base, uint32_t addr, int count);

// D3D Swap: frame boundary.
extern "C" REX_FUNC(sub_823A7420) {
  g_last_world_entries.store(g_world_entries.exchange(0, std::memory_order_relaxed),
                             std::memory_order_relaxed);
  NativeWorldOnSwap();
  if (Enabled()) {
    const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame % 300 == 0) {
      REXLOG_INFO("Native probe frame {}: RenderMesh {} draw-lists {} list-entries {}", frame,
                  g_render_mesh.load(), g_draw_lists.load(), g_draw_list_entries.load());
    }
    g_render_mesh = 0;
    g_draw_lists = 0;
    g_draw_list_entries = 0;
    if (DumpFrame()) {
      g_dumped_mesh = 0;
      g_dumped_list = 0;
    }
  }
  __imp__sub_823A7420(ctx, base);
}

// RenderMesh(?, MeshContext* r4, VertexProgramState* r5).
extern "C" REX_FUNC(sub_82365850) {
  if (Enabled()) {
    g_render_mesh.fetch_add(1, std::memory_order_relaxed);
    if (DumpFrame() && g_dumped_mesh.fetch_add(1) < 3) {
      const uint32_t mc = ctx.r4.u32;
      const uint32_t obj = ReadU32(base, ReadU32(base, mc));  // [[mc+0]+0]
      const uint32_t records = ReadU32(base, mc + 48);
      REXLOG_INFO("Native probe RenderMesh r3={:08X} mc={:08X} vps={:08X} count(+24)={} "
                  "records(+48)={:08X} [[mc]]={:08X} ib={:08X} vb={:08X}\n mc:{}\n "
                  "[[mc]]:{}\n ib:{}\n vb:{}\n record0:{}",
                  ctx.r3.u32, mc, ctx.r5.u32, ReadU32(base, mc + 24), records, obj,
                  ReadU32(base, obj + 48), ReadU32(base, obj + 52), Hex(base, mc, 96),
                  Hex(base, obj, 64), Hex(base, ReadU32(base, obj + 48), 32),
                  Hex(base, ReadU32(base, obj + 52), 32), Hex(base, records, 32));
    }
  }
  __imp__sub_82365850(ctx, base);
}

// SceneRenderView draw list: r3 view, r4 first, r5 count; entries at
// [view+20160], 8 bytes each.
extern "C" REX_FUNC(sub_82364648) {
  g_world_entries.fetch_add(ctx.r5.u32, std::memory_order_relaxed);
  NativeWorldOnDrawList(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (Enabled()) {
    g_draw_lists.fetch_add(1, std::memory_order_relaxed);
    g_draw_list_entries.fetch_add(ctx.r5.u32, std::memory_order_relaxed);
    if (DumpFrame() && g_dumped_list.fetch_add(1) < 3 && ctx.r5.u32) {
      const uint32_t view = ctx.r3.u32;
      const uint32_t list = ReadU32(base, view + 20160);
      const uint32_t entry = list + ctx.r4.u32 * 8;
      const uint32_t a = ReadU32(base, entry + 4);   // MeshContext?
      const uint32_t b = ReadU32(base, a + 4);        // material?
      const uint32_t c = ReadU32(base, b + 108);
      const uint32_t geo = ReadU32(base, ReadU32(base, a));  // [[mc]]
      REXLOG_INFO("Native probe list mc={:08X} count(+24)={} records(+48)={:08X} [[mc]]={:08X} "
                  "ib={:08X} vb={:08X}\n ib:{}\n vb:{}\n record0:{}",
                  a, ReadU32(base, a + 24), ReadU32(base, a + 48), geo, ReadU32(base, geo + 48),
                  ReadU32(base, geo + 52), Hex(base, ReadU32(base, geo + 48), 32),
                  Hex(base, ReadU32(base, geo + 52), 32), Hex(base, ReadU32(base, a + 48), 32));
      // Skate 3 layout candidates: view+0x08 -> camera, camera+0xA0 -> viewproj
      // (16 floats); mesh = [[mc]]: +0x24 material, +0x28 vertex descriptor.
      const uint32_t cam = ReadU32(base, view + 0x08);
      std::string vp;
      for (int i = 0; i < 16; ++i) {
        const uint32_t bits = ReadU32(base, cam + 0xA0 + i * 4);
        float f;
        std::memcpy(&f, &bits, 4);
        char tmp[24];
        std::snprintf(tmp, sizeof(tmp), "%s%.3f", i % 4 ? " " : (i ? " | " : ""), f);
        vp += tmp;
      }
      const uint32_t vdesc = ReadU32(base, geo + 0x28);
      const uint32_t mat = ReadU32(base, geo + 0x24);
      REXLOG_INFO("Native probe camera view+8={:08X} viewproj(+A0): {}\n view:{}\n camera:{}\n "
                  "mesh={:08X}:{}\n vdesc(+28)={:08X}:{}\n material(+24)={:08X}:{}",
                  cam, vp, Hex(base, view, 64), Hex(base, cam, 192), geo, Hex(base, geo, 64), vdesc,
                  Hex(base, vdesc, 96), mat, Hex(base, mat, 96));
      // World-space check: project each entry's mesh bounding-box centre with
      // the camera view-projection, in both multiplication orders.
      {
        float vpm[16];
        for (int i = 0; i < 16; ++i) {
          const uint32_t bits = ReadU32(base, cam + 0xA0 + i * 4);
          std::memcpy(&vpm[i], &bits, 4);
        }
        uint32_t tested = 0, in_row = 0, in_col = 0;
        const uint32_t n = ctx.r5.u32 < 400 ? ctx.r5.u32 : 400;
        for (uint32_t k = 0; k < n; ++k) {
          const uint32_t m = ReadU32(base, list + (ctx.r4.u32 + k) * 8 + 4);
          const uint32_t g = ReadU32(base, ReadU32(base, m));
          float bb[8];
          bool ok = true;
          for (int i = 0; i < 8; ++i) {
            bool good;
            const uint32_t bits = ReadU32(base, g + i * 4, &good);
            ok = ok && good;
            std::memcpy(&bb[i], &bits, 4);
          }
          if (!ok) continue;
          const float c[4] = {(bb[0] + bb[4]) * 0.5f, (bb[1] + bb[5]) * 0.5f,
                              (bb[2] + bb[6]) * 0.5f, 1.0f};
          float r[4], q[4];
          for (int j = 0; j < 4; ++j) {
            r[j] = c[0] * vpm[0 * 4 + j] + c[1] * vpm[1 * 4 + j] + c[2] * vpm[2 * 4 + j] +
                   c[3] * vpm[3 * 4 + j];
            q[j] = vpm[j * 4 + 0] * c[0] + vpm[j * 4 + 1] * c[1] + vpm[j * 4 + 2] * c[2] +
                   vpm[j * 4 + 3] * c[3];
          }
          auto inside = [](const float* v) {
            return v[3] > 0.01f && v[0] >= -1.5f * v[3] && v[0] <= 1.5f * v[3] &&
                   v[1] >= -1.5f * v[3] && v[1] <= 1.5f * v[3];
          };
          ++tested;
          in_row += inside(r);
          in_col += inside(q);
        }
        REXLOG_INFO("Native probe bbox check: tested {} inside(row-vector v*M) {} inside(M*v) {}",
                    tested, in_row, in_col);
      }
      REXLOG_INFO("Native probe draw-list view={:08X} first={} count={} list={:08X} "
                  "entry0:{}\n [e+4]={:08X}:{}\n [[e+4]+4]={:08X}:{}\n [..+108]={:08X}:{}",
                  view, ctx.r4.u32, ctx.r5.u32, list, Hex(base, entry, 16), a, Hex(base, a, 32),
                  b, Hex(base, b, 128), c, Hex(base, c, 64));
    }
  }
  const bool dump_after = Enabled() && DumpFrame() && ctx.r5.u32 > 64;
  const uint32_t view_after = ctx.r3.u32;
  __imp__sub_82364648(ctx, base);
  if (dump_after) {
    const uint32_t bank = g_vs_bank.load(std::memory_order_relaxed);
    const uint32_t cam = ReadU32(base, view_after + 0x08);
    REXLOG_INFO("Native probe matrices after draw list: vs bank {:08X}\n c0..c3: {}\n c4..c7: {}\n "
                "cam+A0: {}\n cam+60: {}\n cam+20: {}", bank, Floats(base, bank, 16),
                Floats(base, bank + 64, 16), Floats(base, cam + 0xA0, 16),
                Floats(base, cam + 0x60, 16), Floats(base, cam + 0x20, 16));
  }
}

// D3D::SetPending_AluConstants(device r3, u64 mask r4, bank r5, ptr r6):
// bank 0x4000 = vertex shader constants; ptr is the device's constant shadow.
extern "C" REX_FUNC(sub_823642E8) {
  if (ctx.r5.u32 == 0x4000 && ctx.r6.u32) g_vs_bank.store(ctx.r6.u32, std::memory_order_relaxed);
  __imp__sub_823642E8(ctx, base);
}

std::string Floats(uint8_t* base, uint32_t addr, int count) {
  std::string s;
  for (int i = 0; i < count; ++i) {
    const uint32_t bits = ReadU32(base, addr + i * 4);
    float f;
    std::memcpy(&f, &bits, 4);
    char tmp[24];
    std::snprintf(tmp, sizeof(tmp), "%s%.3f", i % 4 ? " " : (i ? " | " : ""), f);
    s += tmp;
  }
  return s;
}

// Whether the last completed frame drew the 3D world (scene draw list used).
bool Skate2WorldSceneActive() {
  return g_last_world_entries.load(std::memory_order_relaxed) > 64;
}
