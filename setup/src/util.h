// SPDX-License-Identifier: MIT
// Small Win32 helpers shared by the setup and the launcher. Everything here
// also has to work under Wine / Proton.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace skate2 {

class File {
 public:
  File() = default;
  ~File() { Close(); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  bool OpenRead(const std::wstring& path);
  bool OpenWrite(const std::wstring& path);  // creates or truncates
  void Close();
  bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
  uint64_t Size() const;
  bool ReadAt(uint64_t offset, void* data, size_t size);
  bool Write(const void* data, size_t size);
  bool Flush();

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& text);

std::wstring ExePath();
std::wstring DirName(const std::wstring& path);
std::wstring Join(const std::wstring& dir, const std::wstring& name);
// Lexically absolute path without a trailing separator ("C:\" stays).
std::wstring FullPath(const std::wstring& path);
bool IsFile(const std::wstring& path);
bool IsDir(const std::wstring& path);
bool IsEmptyDir(const std::wstring& path);
// Creates `dir` and its parents. Returns false on failure.
bool MakeDirs(const std::wstring& dir);
std::optional<uint64_t> FreeSpace(const std::wstring& path);

// Printable ASCII only. The prebuilt game opens files through narrow ANSI
// paths, so any other character in the install path breaks it.
bool IsAsciiPath(const std::wstring& path);

// Wine / Proton.
bool IsWine();
// "Z:\home\deck\Games" -> "/home/deck/Games" under Wine, otherwise empty.
std::wstring UnixPath(const std::wstring& path);
// The Linux home folder as a DOS path ("Z:\home\deck") under Wine.
std::wstring WineHome();

// Per-user settings remembered by the setup (HKCU\Software\Skate2PC).
std::wstring RegistryRead(const wchar_t* name);
void RegistryWrite(const wchar_t* name, const std::wstring& value);

// Appends a line to the log file opened by OpenLog.
void OpenLog(const std::wstring& path);
void Log(const wchar_t* format, ...);

std::wstring WindowsError(DWORD code = GetLastError());

}  // namespace skate2
