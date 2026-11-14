// Native renderer entry point.
//
// Registers the guest-output renderer with the SDK (rex/graphics/
// native_guest_renderer.h). The renderer is being ported from Skate 3 Recomp
// by mchughalex (github.com/mchughalex/skate3recomp), used with the author's
// permission. For now it only has a test mode that proves the path end to
// end: --skate2_native_test=true replaces every frame with a slowly changing
// solid color. Otherwise it yields and the emulated GPU output is shown.

#include "native_render.h"
#include "native_world.h"

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/graphics/native_rhi.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>

REXCVAR_DEFINE_BOOL(skate2_native_test, false, "Skate2",
                    "Debug: the native renderer replaces every frame with a solid color")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace nrhi = rex::graphics::nrhi;

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_calls{0};
std::atomic<bool> g_flag_present{false};
std::atomic<bool> g_world_flag_present{false};

// Test mode can also be switched from outside while the game runs: the file
// skate2-native-test.flag next to skate2.exe turns it on (checked about once
// a second), e.g. created and removed over SSH on a Steam Deck.
bool TestFlagFilePresent() {
  if (g_calls.fetch_add(1, std::memory_order_relaxed) % 60 == 0) {
    static const std::wstring path =
        (rex::filesystem::GetExecutableFolder() / "skate2-native-test.flag").wstring();
    g_flag_present.store(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
                         std::memory_order_relaxed);
    static const std::wstring world_path =
        (rex::filesystem::GetExecutableFolder() / "skate2-native-world.flag").wstring();
    g_world_flag_present.store(
        GetFileAttributesW(world_path.c_str()) != INVALID_FILE_ATTRIBUTES,
        std::memory_order_relaxed);
  }
  return g_flag_present.load(std::memory_order_relaxed);
}

bool RenderGuestOutput(const rex::graphics::NativeGuestOutputRenderContext& context,
                       void* /*user_data*/) {
  // Test mode replaces only world frames, so menus stay usable.
  TestFlagFilePresent();  // also refreshes the world flag
  const bool world = g_world_flag_present.load(std::memory_order_relaxed);
  if (Skate2WorldSceneActive() && NativeWorldRender(context, world)) {
    return true;
  }
  const bool test = REXCVAR_GET(skate2_native_test) || g_flag_present.load();
  if (!test || !Skate2WorldSceneActive() || !context.cmd ||
      !context.guest_output) {
    return false;
  }
  const uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed);
  if (frame == 0) {
    REXLOG_INFO("Native renderer: test mode, first frame {}x{}", context.guest_output_width,
                context.guest_output_height);
  }
  const float t = float(frame % 240) / 240.0f * 6.2831853f;
  const float color[4] = {0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t + 2.094f),
                          0.5f + 0.5f * std::sin(t + 4.189f), 1.0f};
  nrhi::Cmd* cmd = context.cmd;
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  cmd->ClearRenderTarget(context.guest_output, color);
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kGuestOutput);
  cmd->FlushBarriers();
  return true;
}

}  // namespace

void InstallSkate2NativeRenderer() {
  rex::graphics::SetNativeGuestOutputRenderer(&RenderGuestOutput, nullptr);
}
