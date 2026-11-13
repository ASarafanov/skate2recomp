// SPDX-License-Identifier: MIT
#include "install.h"

#include "util.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwchar>
#include <sstream>

namespace skate2 {
namespace {

constexpr char kTrailerMagic[8] = {'S', '2', 'S', 'E', 'T', 'U', 'P', '1'};
constexpr size_t kTrailerSize = 32;
constexpr size_t kCopyChunk = 4u << 20;
constexpr uint64_t kMaxIndexSize = 1u << 20;

uint64_t ReadLe64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
  return value;
}

bool SafeRelativeName(const std::string& name) {
  if (name.empty() || name.front() == '/' || name.find("..") != std::string::npos) return false;
  for (char c : name) {
    if (c < 0x20 || c > 0x7E || std::strchr("\\:*?\"<>|", c)) return false;
  }
  return true;
}

class Installer {
 public:
  Installer(const InstallJob& job, InstallProgress& progress, std::wstring& error)
      : job_(job), progress_(progress), error_(error) {}

  InstallResult Run();

 private:
  InstallResult Fail(const std::wstring& message) {
    error_ = message;
    Log(L"Install failed: %ls", message.c_str());
    return InstallResult::kFailed;
  }
  void SetCurrent(const std::wstring& text) {
    std::lock_guard lock(progress_.mutex);
    progress_.current = text;
  }
  // Copies `size` bytes at `offset` of `source` into `destination` through
  // a .part file. Checks the SHA-256 when `sha256` is given.
  InstallResult Copy(File& source, uint64_t offset, uint64_t size, const std::wstring& destination,
                     const std::string& sha256);
  bool WriteMarker();

  const InstallJob& job_;
  InstallProgress& progress_;
  std::wstring& error_;
  std::vector<uint8_t> buffer_ = std::vector<uint8_t>(kCopyChunk);
};

bool Installer::WriteMarker() {
  File marker;
  if (!marker.OpenWrite(Join(job_.target, kMarkerName))) return false;
  std::string text = "Skate 2 install in progress. Run the setup again to finish it.\r\n";
  if (job_.disc) text += "disc=" + Narrow(job_.disc->image) + "\r\n";
  return marker.Write(text.data(), text.size());
}

InstallResult Installer::Copy(File& source, uint64_t offset, uint64_t size,
                              const std::wstring& destination, const std::string& sha256) {
  if (!MakeDirs(DirName(destination))) {
    return Fail(L"Could not create the folder " + DirName(destination) + L": " + WindowsError());
  }
  const std::wstring part = destination + L".part";
  File out;
  if (!out.OpenWrite(part)) return Fail(L"Could not create " + part + L": " + WindowsError());
  Sha256 hash;
  uint64_t remaining = size;
  while (remaining) {
    if (progress_.cancel.load()) {
      out.Close();
      DeleteFileW(part.c_str());
      return InstallResult::kCancelled;
    }
    const size_t chunk = size_t(std::min<uint64_t>(remaining, buffer_.size()));
    if (!source.ReadAt(offset + (size - remaining), buffer_.data(), chunk)) {
      out.Close();
      DeleteFileW(part.c_str());
      return Fail(L"Could not read the source file. If it is on a removable drive, check that it "
                  L"is still connected.");
    }
    if (!sha256.empty()) hash.Update(buffer_.data(), chunk);
    if (!out.Write(buffer_.data(), chunk)) {
      const DWORD code = GetLastError();
      out.Close();
      DeleteFileW(part.c_str());
      return Fail(L"Could not write " + destination + L". Is the drive full? " +
                  WindowsError(code));
    }
    remaining -= chunk;
    progress_.done_bytes += chunk;
  }
  out.Close();
  if (!sha256.empty() && hash.HexDigest() != sha256) {
    DeleteFileW(part.c_str());
    return Fail(L"The setup file is damaged (" + destination +
                L" does not match). Download it again.");
  }
  if (!MoveFileExW(part.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    const DWORD code = GetLastError();
    DeleteFileW(part.c_str());
    if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) {
      return Fail(L"Could not replace " + destination + L". Close Skate 2 and try again.");
    }
    return Fail(L"Could not create " + destination + L": " + WindowsError(code));
  }
  return InstallResult::kDone;
}

InstallResult Installer::Run() {
  const bool resume = IsFile(Join(job_.target, kMarkerName));
  Log(L"Install into %ls (%ls)", job_.target.c_str(),
      !job_.disc ? L"program files only" : resume ? L"resuming" : L"new");
  // Without a disc only the program files change; the game files must be
  // there already, and the folder never looks unfinished.
  if (!job_.disc && !HasGameFiles(job_.target)) {
    return Fail(L"Select your Skate 2 disc image: this folder has no complete game files yet.");
  }
  if (job_.disc && job_.disc->folder &&
      _wcsicmp(FullPath(job_.disc->image).c_str(),
               FullPath(Join(job_.target, kGameDirName)).c_str()) == 0) {
    return Fail(L"These game files are already in the install folder. Choose another source "
                L"or another install folder.");
  }
  if (!MakeDirs(job_.target)) {
    return Fail(L"Could not create the folder " + job_.target + L": " + WindowsError());
  }
  if (job_.disc && !WriteMarker()) {
    return Fail(L"Could not write to " + job_.target + L". Choose another folder.");
  }

  uint64_t total = 0;
  if (job_.payload) total += job_.payload->launcher_size + job_.payload->total_bytes;
  if (job_.disc) total += job_.disc->total_bytes;
  progress_.total_bytes = total;
  progress_.done_bytes = 0;

  // Program files from the payload, then this launcher without it.
  if (job_.payload) {
    File self;
    if (!self.OpenRead(job_.payload->exe)) return Fail(L"Could not read the setup file.");
    for (const PayloadFile& file : job_.payload->files) {
      SetCurrent(Widen(file.name));
      const InstallResult result =
          Copy(self, file.offset, file.size, Join(job_.target, Widen(file.name)), file.sha256);
      if (result != InstallResult::kDone) return result;
    }
    SetCurrent(kLauncherName);
    const InstallResult result =
        Copy(self, 0, job_.payload->launcher_size, Join(job_.target, kLauncherName), {});
    if (result != InstallResult::kDone) return result;
  }

  // Game files from the disc; default.xex comes last.
  if (job_.disc) {
    const std::wstring game_dir = Join(job_.target, kGameDirName);
    const std::wstring xex = Join(game_dir, L"default.xex");
    DeleteFileW(xex.c_str());
    File image;
    if (!job_.disc->folder && !image.OpenRead(job_.disc->image)) {
      return Fail(L"Could not open the disc image: " + WindowsError());
    }
    for (const DiscFile& file : job_.disc->files) {
      const std::wstring destination = Join(game_dir, Widen(file.path));
      if (resume) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(destination.c_str(), GetFileExInfoStandard, &data) &&
            !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            ((uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow) == file.size) {
          progress_.done_bytes += file.size;
          continue;
        }
      }
      SetCurrent(L"game/" + Widen(file.path));
      InstallResult result;
      if (job_.disc->folder) {
        File source;
        if (!source.OpenRead(file.source)) {
          return Fail(L"Could not open " + file.source + L": " + WindowsError());
        }
        if (source.Size() != file.size) {
          return Fail(L"The file changed while installing: " + file.source);
        }
        result = Copy(source, 0, file.size, destination, {});
      } else {
        result = Copy(image, file.offset, file.size, destination, {});
      }
      if (result != InstallResult::kDone) return result;
    }
    if (std::wstring check = CheckXexFile(xex); !check.empty()) {
      DeleteFileW(xex.c_str());
      return Fail(L"The copied default.xex does not match the disc: " + check);
    }
    DeleteFileW(Join(job_.target, kMarkerName).c_str());
  }
  SetCurrent(L"");
  RegistryWrite(L"InstallDir", job_.target);

  const std::wstring launcher = Join(job_.target, kLauncherName);
  if (job_.desktop_shortcut || job_.start_menu_shortcut) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto make = [&](REFKNOWNFOLDERID folder) {
      PWSTR dir = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &dir))) {
        const std::wstring link = Join(dir, L"Skate 2.lnk");
        if (!CreateShortcut(link, launcher, job_.target, L"Play Skate 2")) {
          Log(L"Could not create shortcut %ls", link.c_str());
        }
        CoTaskMemFree(dir);
      }
    };
    if (job_.desktop_shortcut) make(FOLDERID_Desktop);
    if (job_.start_menu_shortcut) make(FOLDERID_Programs);
    CoUninitialize();
  }
  Log(L"Install finished");
  return InstallResult::kDone;
}

}  // namespace

bool ReadPayload(Payload& payload) {
  payload = {};
  payload.exe = ExePath();
  File self;
  if (!self.OpenRead(payload.exe)) return false;
  const uint64_t size = self.Size();
  uint8_t trailer[kTrailerSize];
  if (size < kTrailerSize || !self.ReadAt(size - kTrailerSize, trailer, kTrailerSize) ||
      std::memcmp(trailer, kTrailerMagic, 8) != 0) {
    return false;
  }
  payload.launcher_size = ReadLe64(trailer + 8);
  const uint64_t index_offset = ReadLe64(trailer + 16);
  const uint64_t index_size = ReadLe64(trailer + 24);
  if (index_size > kMaxIndexSize || index_offset + index_size + kTrailerSize > size ||
      payload.launcher_size > index_offset) {
    return false;
  }
  std::string index(size_t(index_size), '\0');
  if (!self.ReadAt(index_offset, index.data(), index.size())) return false;
  // One line per file: "<sha256> <size> <offset> <name>".
  std::istringstream lines(index);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::istringstream fields(line);
    PayloadFile file;
    fields >> file.sha256 >> file.size >> file.offset;
    std::getline(fields >> std::ws, file.name);
    if (file.sha256.size() != 64 || !SafeRelativeName(file.name) ||
        file.offset < payload.launcher_size || file.offset + file.size > index_offset) {
      return false;
    }
    payload.total_bytes += file.size;
    payload.files.push_back(std::move(file));
  }
  return !payload.files.empty();
}

bool HasGameFiles(const std::wstring& dir) {
  const std::wstring game = Join(dir, kGameDirName);
  return IsFile(Join(game, L"default.xex")) && IsDir(Join(game, L"data")) &&
         !IsFile(Join(dir, kMarkerName));
}

bool IsInstalled(const std::wstring& dir) {
  return !dir.empty() && IsFile(Join(dir, kGameExeName)) && HasGameFiles(dir);
}

std::wstring InstallTarget(const std::wstring& folder) {
  const std::wstring full = FullPath(folder);
  const bool ours = !IsDir(full) || IsEmptyDir(full) || IsFile(Join(full, kMarkerName)) ||
                    IsFile(Join(full, kLauncherName)) || IsFile(Join(full, kGameExeName)) ||
                    IsDir(Join(full, kGameDirName));
  return ours ? full : Join(full, L"Skate 2");
}

InstallResult RunInstall(const InstallJob& job, InstallProgress& progress, std::wstring& error) {
  Installer installer(job, progress, error);
  return installer.Run();
}

bool CreateShortcut(const std::wstring& link, const std::wstring& target,
                    const std::wstring& working_dir, const std::wstring& description) {
  IShellLinkW* shell_link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&shell_link)))) {
    return false;
  }
  shell_link->SetPath(target.c_str());
  shell_link->SetWorkingDirectory(working_dir.c_str());
  shell_link->SetDescription(description.c_str());
  IPersistFile* file = nullptr;
  bool ok = false;
  if (SUCCEEDED(shell_link->QueryInterface(IID_PPV_ARGS(&file)))) {
    ok = SUCCEEDED(file->Save(link.c_str(), TRUE));
    file->Release();
  }
  shell_link->Release();
  return ok;
}

}  // namespace skate2
