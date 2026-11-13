// SPDX-License-Identifier: MIT
#include "gamepad.h"

#include <commctrl.h>

#include <cwchar>
#include <initializer_list>

namespace skate2 {
namespace {

struct PadState {
  DWORD packet;
  WORD buttons;
  BYTE left_trigger, right_trigger;
  SHORT lx, ly, rx, ry;
};
using GetStateFn = DWORD(WINAPI*)(DWORD, PadState*);

GetStateFn LoadGetState() {
  static GetStateFn fn = [] {
    for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
      if (HMODULE module = LoadLibraryW(name)) {
        if (auto proc = reinterpret_cast<GetStateFn>(GetProcAddress(module, "XInputGetState"))) {
          return proc;
        }
      }
    }
    return GetStateFn(nullptr);
  }();
  return fn;
}

constexpr SHORT kStickThreshold = 16000;
constexpr DWORD kRepeatDelay = 400, kRepeatRate = 120;

bool IsKind(HWND control, const wchar_t* cls) {
  wchar_t name[32];
  return GetClassNameW(control, name, 32) && _wcsicmp(name, cls) == 0;
}

}  // namespace

uint16_t PadButtons() {
  GetStateFn get = LoadGetState();
  if (!get) return 0;
  uint16_t buttons = 0;
  for (DWORD i = 0; i < 4; ++i) {
    PadState state{};
    if (get(i, &state) != ERROR_SUCCESS) continue;
    buttons |= state.buttons;
    if (state.ly > kStickThreshold) buttons |= kPadUp;
    if (state.ly < -kStickThreshold) buttons |= kPadDown;
    if (state.lx < -kStickThreshold) buttons |= kPadLeft;
    if (state.lx > kStickThreshold) buttons |= kPadRight;
  }
  return buttons;
}

bool PadHeldAtStart(uint16_t button, DWORD wait_ms) {
  if (!LoadGetState()) return false;
  const DWORD start = GetTickCount();
  do {
    if (PadButtons() & button) return true;
    Sleep(25);
  } while (GetTickCount() - start < wait_ms);
  return false;
}

void PadNavigator::Attach(HWND dialog, bool require_foreground) {
  dialog_ = dialog;
  require_foreground_ = require_foreground;
  if (!LoadGetState()) return;
  last_ = PadButtons();  // e.g. View still held from opening the options
  // Show focus rectangles: the controller moves the focus like Tab does.
  SendMessageW(dialog, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL),
               0);
  SetTimer(dialog, kPadTimer, 30, nullptr);
}

void PadNavigator::OnTimer() {
  if (!dialog_ || (require_foreground_ && GetForegroundWindow() != dialog_)) {
    last_ = PadButtons();
    return;
  }
  const uint16_t now = PadButtons();
  const uint16_t pressed = now & ~last_;
  const DWORD tick = GetTickCount();
  for (uint16_t bit : {kPadUp, kPadDown, kPadLeft, kPadRight, kPadA, kPadB, kPadStart}) {
    if (pressed & bit) {
      Press(bit);
      held_ = bit;
      held_since_ = last_repeat_ = tick;
    }
  }
  // Auto-repeat for the directions while held.
  const uint16_t dirs = kPadUp | kPadDown | kPadLeft | kPadRight;
  if ((held_ & dirs) && (now & held_)) {
    if (tick - held_since_ > kRepeatDelay && tick - last_repeat_ > kRepeatRate) {
      Press(held_);
      last_repeat_ = tick;
    }
  } else if (!(now & held_)) {
    held_ = 0;
  }
  last_ = now;
}

void PadNavigator::Press(uint16_t button) {
  HWND focus = GetFocus();
  if (focus && GetParent(focus) != dialog_ && GetParent(GetParent(focus)) == dialog_) {
    focus = GetParent(focus);  // the edit inside a combo box
  }
  switch (button) {
    case kPadUp:
    case kPadDown:
      if (focus && IsKind(focus, WC_COMBOBOXW) &&
          SendMessageW(focus, CB_GETDROPPEDSTATE, 0, 0)) {
        SendMessageW(focus, CB_SHOWDROPDOWN, FALSE, 0);
      }
      SendMessageW(dialog_, WM_NEXTDLGCTL, button == kPadUp ? 1 : 0, FALSE);
      break;
    case kPadLeft:
    case kPadRight:
      if (focus && IsKind(focus, WC_COMBOBOXW)) {
        const LRESULT count = SendMessageW(focus, CB_GETCOUNT, 0, 0);
        LRESULT index = SendMessageW(focus, CB_GETCURSEL, 0, 0) + (button == kPadRight ? 1 : -1);
        if (count > 0 && index >= 0 && index < count) {
          SendMessageW(focus, CB_SETCURSEL, WPARAM(index), 0);
          SendMessageW(dialog_, WM_COMMAND,
                       MAKEWPARAM(GetDlgCtrlID(focus), CBN_SELCHANGE), LPARAM(focus));
        }
      } else {
        SendMessageW(dialog_, WM_NEXTDLGCTL, button == kPadLeft ? 1 : 0, FALSE);
      }
      break;
    case kPadA:
      if (focus && IsKind(focus, WC_BUTTONW) && IsWindowEnabled(focus)) {
        SendMessageW(focus, BM_CLICK, 0, 0);
      }
      break;
    case kPadB:
      SendMessageW(dialog_, WM_COMMAND, IDCANCEL, 0);
      break;
    case kPadStart: {
      const LRESULT id = SendMessageW(dialog_, DM_GETDEFID, 0, 0);
      HWND button_window = HIWORD(id) == DC_HASDEFID ? GetDlgItem(dialog_, LOWORD(id)) : nullptr;
      if (button_window && IsWindowEnabled(button_window)) {
        SendMessageW(button_window, BM_CLICK, 0, 0);
      }
      break;
    }
  }
}

}  // namespace skate2
