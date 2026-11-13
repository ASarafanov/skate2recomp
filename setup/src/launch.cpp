// SPDX-License-Identifier: MIT
#include "launch.h"

#include "install.h"
#include "util.h"

#include <cwchar>

namespace skate2 {

const wchar_t* const kFrameRates[] = {L"original", L"60",  L"120", L"144",
                                      L"165",      L"200", L"240", L"unlimited"};
const size_t kFrameRateCount = sizeof(kFrameRates) / sizeof(kFrameRates[0]);

namespace {

constexpr wchar_t kIniName[] = L"launcher.ini";
constexpr wchar_t kSection[] = L"Launch";

bool ValidFrameRate(const std::wstring& value) {
  for (size_t i = 0; i < kFrameRateCount; ++i) {
    if (value == kFrameRates[i]) return true;
  }
  return false;
}

// Quotes one argument for CommandLineToArgvW / the CRT parser.
std::wstring Quote(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : argument) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  return out + L"\"";
}

}  // namespace

LaunchSettings DefaultSettings() {
  LaunchSettings settings;
  if (IsWine()) {
    // Steam Deck class hardware: 1x internal resolution, paced 60 FPS.
    settings.render_scale = 1;
    settings.frame_rate = L"60";
  }
  return settings;
}

LaunchSettings LoadSettings(const std::wstring& dir) {
  LaunchSettings settings = DefaultSettings();
  const std::wstring ini = Join(dir, kIniName);
  if (!IsFile(ini)) return settings;
  wchar_t value[64];
  const int scale = int(GetPrivateProfileIntW(kSection, L"render_scale", settings.render_scale,
                                              ini.c_str()));
  if (scale >= 1 && scale <= 4) settings.render_scale = scale;
  GetPrivateProfileStringW(kSection, L"frame_rate", settings.frame_rate.c_str(), value, 64,
                           ini.c_str());
  if (ValidFrameRate(value)) settings.frame_rate = value;
  GetPrivateProfileStringW(kSection, L"render_path", settings.render_path.c_str(), value, 64,
                           ini.c_str());
  if (!wcscmp(value, L"rov") || !wcscmp(value, L"rtv")) settings.render_path = value;
  settings.windowed = GetPrivateProfileIntW(kSection, L"windowed", 0, ini.c_str()) != 0;
  return settings;
}

bool SaveSettings(const std::wstring& dir, const LaunchSettings& settings) {
  const std::wstring ini = Join(dir, kIniName);
  const std::wstring scale = std::to_wstring(settings.render_scale);
  return WritePrivateProfileStringW(kSection, L"render_scale", scale.c_str(), ini.c_str()) &&
         WritePrivateProfileStringW(kSection, L"frame_rate", settings.frame_rate.c_str(),
                                    ini.c_str()) &&
         WritePrivateProfileStringW(kSection, L"render_path", settings.render_path.c_str(),
                                    ini.c_str()) &&
         WritePrivateProfileStringW(kSection, L"windowed", settings.windowed ? L"1" : L"0",
                                    ini.c_str());
}

std::vector<std::wstring> GameArguments(const std::wstring& dir, const LaunchSettings& settings,
                                        const std::vector<std::wstring>& extra) {
  const std::wstring scale = std::to_wstring(settings.render_scale);
  // The profile of upstream's Launch-Skate2.ps1 (default 2x / unlimited),
  // with the synchronization settings its frame-rate modes rely on.
  std::vector<std::wstring> args = {
      L"--game_data_root=" + Join(dir, kGameDirName),
      L"--user_data_root=" + Join(dir, L"userdata"),
      L"--cache_root=" + Join(dir, L"cache"),
      L"--gpu_plugin=xenos",
      L"--input_backend=sdl",
      L"--render_target_path_d3d12=" + settings.render_path,
      L"--skate2_frame_rate=" + settings.frame_rate,
      L"--async_shader_compilation=false",
      L"--d3d12_present_idleclean_sync=true",
      L"--resolution_scale=1",
      L"--draw_resolution_scale_x=" + scale,
      L"--draw_resolution_scale_y=" + scale,
  };
  if (settings.windowed) args.push_back(L"--fullscreen=false");
  // Later flags win, so options passed to the launcher override these.
  args.insert(args.end(), extra.begin(), extra.end());
  return args;
}

int RunGame(const std::wstring& dir, const LaunchSettings& settings,
            const std::vector<std::wstring>& extra, HWND owner) {
  const std::wstring exe = Join(dir, kGameExeName);
  std::wstring command = Quote(exe);
  for (const std::wstring& arg : GameArguments(dir, settings, extra)) command += L" " + Quote(arg);
  Log(L"Starting %ls", command.c_str());

  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      dir.c_str(), &startup, &process)) {
    const std::wstring message = L"Skate 2 could not start:\n" + exe + L"\n\n" + WindowsError();
    Log(L"%ls", message.c_str());
    MessageBoxW(owner, message.c_str(), L"Skate 2", MB_ICONERROR);
    return 1;
  }
  CloseHandle(process.hThread);
  // Wait like upstream's launcher: Steam tracks the process it started.
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  Log(L"Skate 2 exited with code %lu", code);
  if (code != 0) {
    wchar_t text[1024];
    swprintf(text, 1024,
             L"Skate 2 exited with code %lu (0x%08lX).\n\nThe game's log is in the logs folder "
             L"of %ls.",
             code, code, dir.c_str());
    MessageBoxW(owner, text, L"Skate 2", MB_ICONWARNING);
  }
  return int(code);
}

}  // namespace skate2
