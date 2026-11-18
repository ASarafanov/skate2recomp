// Per-draw render state of the game's emulated draws.
//
// The command processor reports every draw it parses through the SDK's
// emulated draw observer, before the suppression early-out, so this works
// while the native renderer replaces the frame. Here the draws into the main
// scene colour target are kept by (vertex buffer, first index), and the native
// world renderer looks its meshes up by the same addresses. This gives the
// exact alpha test, blending, culling and polygon offset the game set, without
// reverse-engineering its effect objects, plus the world shader's per-frame
// constants (fog, exposure).
//
// Everything runs on the command processor thread (the observer and the
// native renderer), so there is no locking.

#include "native_draw_state.h"

#include <rex/graphics/native_guest_renderer.h>

#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// The game's main world pixel shader (ucode hash); its constants carry the
// per-frame fog and exposure.
constexpr uint64_t kWorldPixelShader = 0x5D2EA5FF45C0BC9Aull;
// The main world material (normal, detail and macro maps, sun specular,
// environment): its vertex shader carries the UV scales, its pixel shader the
// sun direction.
constexpr uint64_t kMaterialVertexShader = 0xE5C957221BEF0ECDull;
constexpr uint64_t kMaterialPixelShader = 0xF8D56B275E4DE57Bull;
// The final lens pass: barrel distortion of the scene and a vignette texture.
constexpr uint64_t kLensPixelShader = 0x9D5F41FC68105CB5ull;
// The final composite before it: scene * c254.x + bloom, then a 3D colour
// grading table.
constexpr uint64_t kGradePixelShader = 0xE063C9ADE96048FBull;

struct Entry {
  NativeDrawState state;
  uint32_t frame = 0;
};

struct Constants {
  uint32_t frame = 0;
  uint64_t vertex_shader = 0;
  uint64_t pixel_shader = 0;
  std::array<float, 256 * 4> vs;
  std::array<float, 256 * 4> ps;
};

std::unordered_map<uint64_t, Entry> g_states;
std::unordered_set<uint64_t> g_wanted;
std::unordered_map<uint64_t, Constants> g_constants_by_draw;
uint32_t g_frame = 1;
// Draw count per render target pitch this frame; the main scene target is the
// busiest one of the previous frame (shadow, reflection and lightmap-page
// passes redraw the same buffers with other state).
std::vector<std::pair<uint32_t, uint32_t>> g_pitch_counts;
uint32_t g_main_pitch = 0;
NativeFrameConstants g_constants;
uint32_t g_lens_frame = 0;
uint32_t g_grade_frame = 0;

NativeDrawState::Cull CullFromMode(uint32_t mode) {
  const bool cull_front = mode & 1, cull_back = mode & 2;
  // Xenos face bit: 0 = counter-clockwise faces are front. The native RHI
  // keeps D3D's clockwise front faces.
  const bool ccw_front = (mode & 4) == 0;
  if (cull_back) return ccw_front ? NativeDrawState::kFront : NativeDrawState::kBack;
  if (cull_front) return ccw_front ? NativeDrawState::kBack : NativeDrawState::kFront;
  return NativeDrawState::kNone;
}

void Observe(const rex::graphics::NativeEmulatedDraw& d, void* /*user_data*/) {
  if (d.pixel_shader_hash == kLensPixelShader && d.ps_constants && d.fetch_constants) {
    std::memcpy(g_constants.lens, d.ps_constants, sizeof(float) * 8);
    std::memcpy(g_constants.lens + 8, d.ps_constants + 254 * 4, sizeof(float) * 8);
    std::memcpy(g_constants.lens_vignette, d.fetch_constants + 2 * 6,
                sizeof(g_constants.lens_vignette));
    g_constants.lens_valid = true;
    g_lens_frame = g_frame;
  }
  if (d.pixel_shader_hash == kGradePixelShader && d.ps_constants && d.fetch_constants) {
    std::memcpy(g_constants.grade, d.ps_constants, sizeof(float) * 12);
    std::memcpy(g_constants.grade + 12, d.ps_constants + 254 * 4, sizeof(float) * 8);
    std::memcpy(g_constants.grade_lut, d.fetch_constants + 4 * 6,
                sizeof(g_constants.grade_lut));
    g_constants.grade_valid = true;
    g_grade_frame = g_frame;
  }
  if (!d.pixel_shader_hash || (d.color_mask & 0xF) == 0 || !d.vertex_buffer) return;
  bool counted = false;
  for (auto& p : g_pitch_counts) {
    if (p.first == d.surface_pitch) {
      ++p.second;
      counted = true;
      break;
    }
  }
  if (!counted) g_pitch_counts.emplace_back(d.surface_pitch, 1);
  if (g_main_pitch && d.surface_pitch != g_main_pitch) return;

  if (d.pixel_shader_hash == kWorldPixelShader && d.vs_constants && d.ps_constants) {
    std::memcpy(g_constants.fog, d.vs_constants + 5 * 4, sizeof(g_constants.fog));
    std::memcpy(g_constants.fog_color, d.vs_constants + 6 * 4, sizeof(g_constants.fog_color));
    g_constants.exposure = d.ps_constants[6 * 4];
    g_constants.valid = true;
  }
  if (d.pixel_shader_hash == kMaterialPixelShader && d.ps_constants) {
    const float* sun = d.ps_constants + 4 * 4;
    if (std::isfinite(sun[0]) && std::isfinite(sun[1]) && std::isfinite(sun[2])) {
      std::memcpy(g_constants.sun, sun, sizeof(g_constants.sun));
      g_constants.sun_valid = true;
    }
    bool finite = true;
    for (int k = 0; k < 16; ++k) finite = finite && std::isfinite(d.ps_constants[k]);
    if (finite) {
      std::memcpy(g_constants.shadow, d.ps_constants, sizeof(g_constants.shadow));
      g_constants.shadow_valid = true;
    }
  }

  const uint64_t key = uint64_t(d.vertex_buffer) << 32 | d.index_base;
  if (d.vs_constants && g_wanted.count(key)) {
    Constants& c = g_constants_by_draw[key];
    c.frame = g_frame;
    c.vertex_shader = d.vertex_shader_hash;
    c.pixel_shader = d.pixel_shader_hash;
    std::memcpy(c.vs.data(), d.vs_constants, sizeof(float) * c.vs.size());
    if (d.ps_constants) {
      std::memcpy(c.ps.data(), d.ps_constants, sizeof(float) * c.ps.size());
    } else {
      c.ps.fill(0.0f);
    }
  }
  Entry& e = g_states[key];
  e.frame = g_frame;
  NativeDrawState& s = e.state;
  // RB_COLORCONTROL: alpha function (bits 0-2), alpha test enable (bit 3).
  // The world shaders use GEQUAL (6); others are treated as "off".
  const uint32_t func = d.color_control & 7;
  s.alpha_ref = (d.color_control & 8) && func == 6 ? (d.alpha_ref > 0.0f ? d.alpha_ref : 1e-6f)
                                                   : 0.0f;
  // RB_BLENDCONTROL0 colour source (bits 0-4) / destination (bits 8-12):
  // 1 = one, 0 = zero, 6 = src alpha, 7 = 1 - src alpha.
  const uint32_t src = d.blend_control & 0x1F, dst = (d.blend_control >> 8) & 0x1F;
  if (src == 6 && dst == 7) {
    s.blend = NativeDrawState::kAlpha;
  } else if (dst == 1) {
    s.blend = NativeDrawState::kAdditive;
  } else {
    s.blend = NativeDrawState::kOpaque;
  }
  s.cull = CullFromMode(d.mode_control);
  s.depth_write = (d.depth_control & 4) != 0;
  s.depth_offset = (d.mode_control & 0x1800) != 0;
  s.pixel_shader = d.pixel_shader_hash;
  if (d.vertex_shader_hash == kMaterialVertexShader && d.vs_constants) {
    s.macro_scale = d.vs_constants[7 * 4];
    s.detail_scale = d.vs_constants[8 * 4];
  } else {
    s.macro_scale = s.detail_scale = 0.0f;
  }
}

}  // namespace

void InstallSkate2DrawObserver() {
  rex::graphics::SetNativeEmulatedDrawObserver(&Observe, nullptr);
}

void NativeDrawStateNewFrame() {
  uint32_t best = 0;
  for (const auto& p : g_pitch_counts) {
    if (p.second > best) {
      best = p.second;
      g_main_pitch = p.first;
    }
  }
  g_pitch_counts.clear();
  ++g_frame;
  if (g_frame - g_lens_frame > 2) g_constants.lens_valid = false;
  if (g_frame - g_grade_frame > 2) g_constants.grade_valid = false;
  // World streaming reuses buffer addresses: drop stale entries now and then.
  if (g_wanted.size() > 4096) g_wanted.clear();
  if (g_constants_by_draw.size() > 512) {
    for (auto it = g_constants_by_draw.begin(); it != g_constants_by_draw.end();) {
      it = g_frame - it->second.frame > 2 ? g_constants_by_draw.erase(it) : std::next(it);
    }
  }
  if (g_states.size() > 32768) {
    for (auto it = g_states.begin(); it != g_states.end();) {
      it = g_frame - it->second.frame > 2 ? g_states.erase(it) : std::next(it);
    }
  }
}

bool NativeDrawStateLookup(uint32_t vertex_buffer, uint32_t index_base, NativeDrawState& out) {
  auto it = g_states.find(uint64_t(vertex_buffer) << 32 | index_base);
  if (it == g_states.end() || g_frame - it->second.frame > 2) return false;
  out = it->second.state;
  return true;
}

const NativeFrameConstants& NativeDrawFrameConstants() { return g_constants; }

void NativeDrawStateWantConstants(uint32_t vertex_buffer, uint32_t index_base) {
  g_wanted.insert(uint64_t(vertex_buffer) << 32 | index_base);
}

bool NativeDrawStateConstants(uint32_t vertex_buffer, uint32_t index_base,
                              NativeDrawConstants& out) {
  auto it = g_constants_by_draw.find(uint64_t(vertex_buffer) << 32 | index_base);
  if (it == g_constants_by_draw.end() || g_frame - it->second.frame > 2) return false;
  out.vertex_shader = it->second.vertex_shader;
  out.pixel_shader = it->second.pixel_shader;
  out.vs = it->second.vs.data();
  out.ps = it->second.ps.data();
  return true;
}
