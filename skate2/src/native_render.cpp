// Native renderer entry point.
//
// Registers the guest-output renderer with the SDK (rex/graphics/
// native_guest_renderer.h). The renderer is being ported from Skate 3 Recomp
// by mchughalex (github.com/mchughalex/skate3recomp), used with the author's
// permission. For now it only has a test mode that proves the path end to
// end: --skate2_native_test=true replaces every frame with a slowly changing
// solid color. Otherwise it yields and the emulated GPU output is shown.

#include "native_render.h"

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/graphics/native_rhi.h>
#include <rex/logging.h>

#include <atomic>
#include <cmath>
#include <cstdint>

REXCVAR_DEFINE_BOOL(skate2_native_test, false, "Skate2",
                    "Debug: the native renderer replaces every frame with a solid color")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace nrhi = rex::graphics::nrhi;

std::atomic<uint64_t> g_frames{0};

bool RenderGuestOutput(const rex::graphics::NativeGuestOutputRenderContext& context,
                       void* /*user_data*/) {
  if (!REXCVAR_GET(skate2_native_test) || !context.cmd || !context.guest_output) {
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
