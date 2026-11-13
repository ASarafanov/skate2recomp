// SPDX-License-Identifier: MIT
// Controller support for the launcher's windows, so they work on a Steam
// Deck in Game Mode (no keyboard): XInput, which Steam Input provides under
// Proton too. Loaded at run time; without it everything is a no-op.
#pragma once

#include <windows.h>

#include <cstdint>

namespace skate2 {

// XInput button bits.
inline constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004,
                          kPadRight = 0x0008, kPadStart = 0x0010, kPadBack = 0x0020,
                          kPadA = 0x1000, kPadB = 0x2000;

// Buttons held on any connected controller, with the left stick folded into
// the d-pad bits. 0 when no controller or no XInput.
uint16_t PadButtons();

// True when a controller holds View/Back now or within `wait_ms` (a pad can
// take a moment to show up right after the process starts).
bool PadHeldAtStart(uint16_t button, DWORD wait_ms);

// Lets a controller drive a dialog: up/down move between controls,
// left/right change a combo box, A presses the focused button or toggles a
// check box, B is Cancel, Start is the default button. Call Attach from
// WM_INITDIALOG; it polls on a timer of the dialog (id kPadTimer) and the
// dialog forwards WM_TIMER with that id to OnTimer.
class PadNavigator {
 public:
  static constexpr UINT_PTR kPadTimer = 0x5AD;
  // require_foreground: ignore the controller while another window is active
  // (the setup); the launcher window always listens.
  void Attach(HWND dialog, bool require_foreground);
  void OnTimer();

 private:
  void Press(uint16_t button);
  HWND dialog_ = nullptr;
  bool require_foreground_ = true;
  uint16_t last_ = 0;
  uint16_t held_ = 0;
  DWORD held_since_ = 0;
  DWORD last_repeat_ = 0;
};

}  // namespace skate2
