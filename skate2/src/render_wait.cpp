// Gentler GPU waits for the game's render thread under Wine.
//
// sub_82A5A970 is the game's D3D "wait for the GPU" step: it spins for a few
// hundred console cycles (db16cyc, which the recompiled code drops), checks
// whether the command processor has caught up, and returns 1 while the wait
// should go on (0 when done or after its hang timeout). Its callers call it in
// a loop, so on the PC the render thread spins on a whole core while the
// emulated GPU works. On the Steam Deck that core's power comes out of the
// same budget as the GPU's, so the spin lowers the GPU clock and the frame
// rate. Under Wine, back off progressively while the wait goes on: short
// waits keep their latency, long ones sleep in 50 us steps. On Windows the
// game behaves exactly as before.

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <immintrin.h>
#include <windows.h>

#include <cstdint>

REX_EXTERN(__imp__sub_82A5A970);

namespace {

using NtDelayExecutionFunction = LONG(NTAPI*)(BOOLEAN alertable, const LARGE_INTEGER* interval);

struct WineBackoff {
  bool enabled = false;
  NtDelayExecutionFunction delay = nullptr;
  int64_t new_wait_ticks = 0;  // A gap longer than this starts a new wait.
};

const WineBackoff& Backoff() {
  static const WineBackoff backoff = [] {
    WineBackoff b;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return b;
    b.delay = reinterpret_cast<NtDelayExecutionFunction>(GetProcAddress(ntdll, "NtDelayExecution"));
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    b.new_wait_ticks = frequency.QuadPart / 1000;  // 1 ms
    b.enabled = b.delay != nullptr;
    return b;
  }();
  return backoff;
}

thread_local uint32_t t_checks = 0;
thread_local int64_t t_last_check = 0;

void BackOff(const WineBackoff& backoff) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (now.QuadPart - t_last_check > backoff.new_wait_ticks) t_checks = 0;
  t_last_check = now.QuadPart;
  const uint32_t checks = ++t_checks;
  if (checks <= 64) {
    // Roughly what the console's delay loop takes: stay responsive.
    for (int i = 0; i < 8; ++i) _mm_pause();
  } else if (checks <= 256) {
    SwitchToThread();
  } else {
    const LARGE_INTEGER interval{.QuadPart = -500};  // 50 us, relative
    backoff.delay(FALSE, &interval);
  }
}

}  // namespace

extern "C" REX_FUNC(sub_82A5A970) {
  __imp__sub_82A5A970(ctx, base);
  if (ctx.r3.u32 != 1) return;
  const WineBackoff& backoff = Backoff();
  if (backoff.enabled) BackOff(backoff);
}
