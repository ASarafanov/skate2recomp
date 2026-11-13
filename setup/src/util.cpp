// SPDX-License-Identifier: MIT
#include "util.h"

#include <cstdarg>
#include <cwchar>
#include <mutex>

namespace skate2 {

bool File::OpenRead(const std::wstring& path) {
  Close();
  handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  return IsOpen();
}

bool File::OpenWrite(const std::wstring& path) {
  Close();
  handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  return IsOpen();
}

void File::Close() {
  if (IsOpen()) CloseHandle(handle_);
  handle_ = INVALID_HANDLE_VALUE;
}

uint64_t File::Size() const {
  LARGE_INTEGER size{};
  return IsOpen() && GetFileSizeEx(handle_, &size) ? uint64_t(size.QuadPart) : 0;
}

bool File::ReadAt(uint64_t offset, void* data, size_t size) {
  LARGE_INTEGER position;
  position.QuadPart = LONGLONG(offset);
  if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) return false;
  auto* out = static_cast<uint8_t*>(data);
  while (size) {
    const DWORD chunk = DWORD(size > (1u << 30) ? (1u << 30) : size);
    DWORD read = 0;
    if (!ReadFile(handle_, out, chunk, &read, nullptr) || read == 0) return false;
    out += read;
    size -= read;
  }
  return true;
}

bool File::Write(const void* data, size_t size) {
  auto* in = static_cast<const uint8_t*>(data);
  while (size) {
    const DWORD chunk = DWORD(size > (1u << 30) ? (1u << 30) : size);
    DWORD written = 0;
    if (!WriteFile(handle_, in, chunk, &written, nullptr) || written == 0) return false;
    in += written;
    size -= written;
  }
  return true;
}

bool File::Flush() { return FlushFileBuffers(handle_) != 0; }

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring text(size_t(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), text.data(), count);
  return text;
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0,
                                        nullptr, nullptr);
  std::string bytes(size_t(count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), count, nullptr,
                      nullptr);
  return bytes;
}

std::wstring ExePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (length < path.size()) {
      path.resize(length);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

std::wstring DirName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return L".";
  if (slash == 2 && path[1] == L':') return path.substr(0, 3);
  return path.substr(0, slash);
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  std::wstring out = dir;
  if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
  for (wchar_t c : name) out += (c == L'/') ? L'\\' : c;
  return out;
}

std::wstring FullPath(const std::wstring& path) {
  if (path.empty()) return {};
  const DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!length) return path;
  std::wstring out(length, L'\0');
  const DWORD written = GetFullPathNameW(path.c_str(), length, out.data(), nullptr);
  out.resize(written);
  while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
  return out;
}

bool IsFile(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsDir(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsEmptyDir(const std::wstring& path) {
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(Join(path, L"*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return true;
  bool empty = true;
  do {
    if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..")) {
      empty = false;
      break;
    }
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return empty;
}

bool MakeDirs(const std::wstring& dir) {
  if (dir.empty() || IsDir(dir)) return true;
  const std::wstring parent = DirName(dir);
  if (parent != dir && !MakeDirs(parent)) return false;
  return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::optional<uint64_t> FreeSpace(const std::wstring& path) {
  std::wstring dir = FullPath(path);
  while (!dir.empty() && !IsDir(dir)) {
    const std::wstring parent = DirName(dir);
    if (parent == dir) return std::nullopt;
    dir = parent;
  }
  ULARGE_INTEGER available{};
  if (!GetDiskFreeSpaceExW(Join(dir, L"").c_str(), &available, nullptr, nullptr)) {
    return std::nullopt;
  }
  return uint64_t(available.QuadPart);
}

bool IsAsciiPath(const std::wstring& path) {
  for (wchar_t c : path) {
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

bool IsWine() {
  static const bool wine = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }();
  return wine;
}

std::wstring UnixPath(const std::wstring& path) {
  if (!IsWine()) return {};
  using Fn = char*(CDECL*)(const wchar_t*);
  auto fn = reinterpret_cast<Fn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name"));
  if (!fn) return {};
  char* unix_path = fn(path.c_str());
  if (!unix_path) return {};
  std::wstring out = Widen(unix_path);
  HeapFree(GetProcessHeap(), 0, unix_path);
  return out;
}

std::wstring WineHome() {
  if (!IsWine()) return {};
  // Wine exports the Unix home as \??\Z:\home\<user>.
  wchar_t value[1024];
  DWORD length = GetEnvironmentVariableW(L"WINEHOMEDIR", value, 1024);
  if (length && length < 1024) {
    std::wstring home = value;
    if (home.rfind(L"\\??\\", 0) == 0) home = home.substr(4);
    if (IsDir(home)) return FullPath(home);
  }
  // Fallback: the Unix HOME variable, converted to a DOS path.
  char unix_home[1024];
  length = GetEnvironmentVariableA("HOME", unix_home, 1024);
  if (length && length < 1024 && unix_home[0] == '/') {
    using Fn = wchar_t*(CDECL*)(const char*);
    auto fn = reinterpret_cast<Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
    if (fn) {
      if (wchar_t* dos = fn(unix_home)) {
        std::wstring home = dos;
        HeapFree(GetProcessHeap(), 0, dos);
        if (IsDir(home)) return FullPath(home);
      }
    }
  }
  return {};
}

namespace {
constexpr wchar_t kRegistryKey[] = L"Software\\Skate2PC";
}

std::wstring RegistryRead(const wchar_t* name) {
  wchar_t value[2048];
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, name, RRF_RT_REG_SZ, nullptr, value, &size) !=
      ERROR_SUCCESS) {
    return {};
  }
  return value;
}

void RegistryWrite(const wchar_t* name, const std::wstring& value) {
  HKEY key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                 DWORD((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
}

namespace {
std::mutex g_log_mutex;
FILE* g_log = nullptr;
}  // namespace

void OpenLog(const std::wstring& path) {
  std::lock_guard lock(g_log_mutex);
  if (g_log) fclose(g_log);
  g_log = _wfopen(path.c_str(), L"ab");
}

void Log(const wchar_t* format, ...) {
  wchar_t text[2048];
  va_list args;
  va_start(args, format);
  vswprintf(text, 2048, format, args);
  va_end(args);
  SYSTEMTIME now;
  GetLocalTime(&now);
  char stamp[32];
  snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d ", now.wYear, now.wMonth, now.wDay,
           now.wHour, now.wMinute, now.wSecond);
  std::lock_guard lock(g_log_mutex);
  if (!g_log) return;
  fputs(stamp, g_log);
  fputs(Narrow(text).c_str(), g_log);
  fputs("\r\n", g_log);
  fflush(g_log);
}

std::wstring WindowsError(DWORD code) {
  wchar_t* message = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
  std::wstring text = message ? message : L"";
  if (message) LocalFree(message);
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) {
    text.pop_back();
  }
  wchar_t code_text[32];
  swprintf(code_text, 32, L" (error %lu)", code);
  return text + code_text;
}

}  // namespace skate2
