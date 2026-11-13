// Hardware fused multiply-add for the recompiled game code.
//
// The generated code calls fma() for every PowerPC fmadd/fmsub family
// instruction. The build targets x86-64-v2, which has no FMA instruction, so
// these become calls into the C runtime. Microsoft's ucrtbase uses the FMA3
// instruction when the CPU has it, but Wine's (Proton's) ucrtbase computes fma
// in software, which made it the hottest function of the audio thread on the
// Steam Deck. This definition takes the place of the CRT import: one FMA3
// instruction when the CPU and OS support it, otherwise the CRT's own fma.
// Both give the exactly rounded fused result, so behavior is unchanged.
//
// <cmath> is deliberately not included: it declares fma as a DLL import.

#include <intrin.h>
#include <windows.h>

#include <cstdint>
#include <initializer_list>

namespace {

using FmaFunction = double (*)(double, double, double);

__attribute__((target("fma"))) double FmaHardware(double x, double y, double z) {
  return __builtin_fma(x, y, z);
}

bool CpuHasFma() {
  int regs[4] = {};
  __cpuid(regs, 1);
  const uint32_t ecx = uint32_t(regs[2]);
  const bool fma = ecx & (1u << 12);
  const bool osxsave = ecx & (1u << 27);
  const bool avx = ecx & (1u << 28);
  if (!fma || !osxsave || !avx) return false;
  // The OS must save the YMM state (XCR0 bits 1 and 2) for VEX instructions.
  return (_xgetbv(0) & 6) == 6;
}

FmaFunction CrtFma() {
  for (const wchar_t* name : {L"ucrtbase.dll", L"api-ms-win-crt-math-l1-1-0.dll"}) {
    if (const HMODULE module = GetModuleHandleW(name)) {
      if (const FARPROC proc = GetProcAddress(module, "fma")) {
        return reinterpret_cast<FmaFunction>(proc);
      }
    }
  }
  return nullptr;
}

double FmaResolve(double x, double y, double z);

FmaFunction g_fma = FmaResolve;

double FmaResolve(double x, double y, double z) {
  FmaFunction selected = CpuHasFma() ? FmaHardware : CrtFma();
  if (!selected) selected = FmaHardware;  // Unreachable with the DLL CRT.
  g_fma = selected;
  return selected(x, y, z);
}

}  // namespace

extern "C" double fma(double x, double y, double z) {
  return g_fma(x, y, z);
}
