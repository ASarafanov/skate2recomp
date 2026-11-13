// SPDX-License-Identifier: MIT
// Starts skate2.exe with the options of upstream's Launch-Skate2.ps1, without
// PowerShell, so it also runs under Wine / Proton.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace skate2 {

struct LaunchSettings {
  int render_scale = 2;                // 1..4
  std::wstring frame_rate = L"unlimited";  // original, 60, 120, ... unlimited
  std::wstring render_path = L"rov";   // rov or rtv
  bool windowed = false;
  bool show_options = false;           // open the options on every start
};

extern const wchar_t* const kFrameRates[];
extern const size_t kFrameRateCount;

// Defaults: 2x / unlimited on Windows (upstream), 1x / 60 under Proton.
LaunchSettings DefaultSettings();
// From <dir>\launcher.ini, falling back to the defaults.
LaunchSettings LoadSettings(const std::wstring& dir);
bool SaveSettings(const std::wstring& dir, const LaunchSettings& settings);

// The skate2.exe command line (without the exe itself).
std::vector<std::wstring> GameArguments(const std::wstring& dir, const LaunchSettings& settings,
                                        const std::vector<std::wstring>& extra);

// Starts the game in `dir`, waits for it and returns its exit code; shows a
// message box when it cannot start or exits with an error.
int RunGame(const std::wstring& dir, const LaunchSettings& settings,
            const std::vector<std::wstring>& extra, HWND owner);

}  // namespace skate2
