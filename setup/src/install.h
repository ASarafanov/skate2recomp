// SPDX-License-Identifier: MIT
// The setup is the launcher exe with the program files appended to it (the
// payload, written by tools/package_setup.py). Installing copies the launcher
// part of the exe, the payload and the game files from the player's disc into
// one folder:
//
//   <folder>\Launch Skate 2.exe   this launcher, without the payload
//   <folder>\skate2.exe, *.dll    the recompiled game and its runtime
//   <folder>\game\                default.xex and data\ from the disc
//   <folder>\userdata, cache      created by the game
#pragma once

#include "disc.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace skate2 {

inline constexpr wchar_t kLauncherName[] = L"Launch Skate 2.exe";
inline constexpr wchar_t kGameExeName[] = L"skate2.exe";
inline constexpr wchar_t kGameDirName[] = L"game";
// Present while an install into the folder is unfinished.
inline constexpr wchar_t kMarkerName[] = L".skate2-install";

struct PayloadFile {
  std::string name;  // '/' separated, relative to the install folder
  uint64_t offset = 0;  // in the setup exe
  uint64_t size = 0;
  std::string sha256;
};

struct Payload {
  std::wstring exe;
  uint64_t launcher_size = 0;  // the exe without the payload
  std::vector<PayloadFile> files;
  uint64_t total_bytes = 0;
};

// Reads the payload index appended to this exe. False for the plain launcher.
bool ReadPayload(Payload& payload);

// True when `dir` holds a finished install (game exe and game files).
bool IsInstalled(const std::wstring& dir);
// True when `dir` holds the game files from the disc, possibly without the
// program files (an update needs no disc then).
bool HasGameFiles(const std::wstring& dir);

// Where the files go when the player picks `folder`: the folder itself when
// it is missing, empty or already holds Skate 2, otherwise a "Skate 2"
// folder inside it.
std::wstring InstallTarget(const std::wstring& folder);

struct InstallJob {
  std::wstring target;
  const Payload* payload = nullptr;  // null: game files only (portable use)
  std::optional<Disc> disc;          // empty: keep the game files already there
  bool desktop_shortcut = false;
  bool start_menu_shortcut = false;
};

struct InstallProgress {
  std::atomic<uint64_t> done_bytes{0};
  std::atomic<uint64_t> total_bytes{0};
  std::atomic<bool> cancel{false};
  std::mutex mutex;
  std::wstring current;  // guarded by mutex
};

enum class InstallResult { kDone, kCancelled, kFailed };

// Runs on a worker thread. Files are written as <name>.part and renamed
// when complete; a cancelled or failed install keeps what it finished and
// the next run with the same disc resumes it.
InstallResult RunInstall(const InstallJob& job, InstallProgress& progress, std::wstring& error);

bool CreateShortcut(const std::wstring& link, const std::wstring& target,
                    const std::wstring& working_dir, const std::wstring& description);

}  // namespace skate2
