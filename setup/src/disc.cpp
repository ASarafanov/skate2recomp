// SPDX-License-Identifier: MIT
#include "disc.h"

#include "util.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <set>

namespace skate2 {
namespace {

constexpr uint64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
// Game partition offsets: plain XISO, XGD3, XGD2, XGD1.
constexpr std::array<uint64_t, 4> kPartitionOffsets = {0, 0x2080000, 0xFD90000, 0x18300000};
constexpr uint8_t kAttrDirectory = 0x10;
// The console dashboard update on the disc, not game data.
constexpr char kSkippedDir[] = "$systemupdate/";
constexpr uint32_t kMaxDirectorySize = 16u << 20;
constexpr uint64_t kMaxXexSize = 64ull << 20;
constexpr size_t kMaxEntries = 100000;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
  return text;
}

// A file name from the disc that is safe to create: printable ASCII, no
// separators or characters Windows forbids, not "." or "..".
bool SafeName(const std::string& name) {
  if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') {
    return false;
  }
  for (char c : name) {
    if (c < 0x20 || c > 0x7E || std::strchr("\\/:*?\"<>|", c)) return false;
  }
  return true;
}

struct XexIds {
  uint32_t title_id = 0;
  uint32_t media_id = 0;
};

// Title and media ID from the XEX2 execution info header.
XexIds ReadXexIds(const std::vector<uint8_t>& data) {
  if (data.size() < 24 || std::memcmp(data.data(), "XEX2", 4) != 0) return {};
  const uint32_t count = Be32(data.data() + 20);
  for (uint64_t i = 0; i < count && 24 + i * 8 + 8 <= data.size(); ++i) {
    const uint8_t* header = data.data() + 24 + i * 8;
    if (Be32(header) == 0x00040006) {
      const uint64_t value = Be32(header + 4);
      if (value + 16 > data.size()) return {};
      return {Be32(data.data() + value + 12), Be32(data.data() + value)};
    }
  }
  return {};
}

std::wstring CheckXex(const std::vector<uint8_t>& data) {
  const XexIds ids = ReadXexIds(data);
  if (ids.title_id != kTitleId) {
    if (!ids.title_id) return L"default.xex is not an Xbox 360 executable.";
    wchar_t text[160];
    swprintf(text, 160, L"This is not Skate 2 (title ID %08X).", ids.title_id);
    return text;
  }
  Sha256 hash;
  hash.Update(data.data(), data.size());
  if (hash.HexDigest() != kXexSha256) {
    wchar_t text[400];
    swprintf(text, 400,
             L"This is Skate 2, but a different release (media ID %08X). Only the release with "
             L"media ID %08X is supported: the game code was compiled from it.",
             ids.media_id, kMediaId);
    return text;
  }
  return {};
}

std::wstring ReadFileTable(File& in, uint64_t image_size, std::vector<DiscFile>& files) {
  uint64_t base = 0;
  uint32_t root_sector = 0, root_size = 0;
  bool found = false;
  for (uint64_t offset : kPartitionOffsets) {
    uint8_t header[28];
    if (in.ReadAt(offset + 32 * kSector, header, sizeof(header)) &&
        std::memcmp(header, kMagic, sizeof(kMagic) - 1) == 0) {
      base = offset;
      root_sector = Le32(header + 20);
      root_size = Le32(header + 24);
      found = true;
      break;
    }
  }
  if (!found) return L"This is not an Xbox 360 disc image.";

  struct Directory {
    uint32_t sector, size;
    std::string prefix;
  };
  std::vector<Directory> directories{{root_sector, root_size, ""}};
  size_t entries = 0;
  while (!directories.empty()) {
    const Directory dir = std::move(directories.back());
    directories.pop_back();
    if (dir.size == 0) continue;
    if (dir.size > kMaxDirectorySize || dir.prefix.size() > 400) {
      return L"The disc image is damaged (bad directory).";
    }
    std::vector<uint8_t> data(dir.size);
    if (!in.ReadAt(base + uint64_t(dir.sector) * kSector, data.data(), data.size())) {
      return L"The disc image is incomplete or damaged.";
    }
    // Each directory is a binary tree of entries; offsets count 4 bytes.
    std::vector<uint32_t> stack{0};
    std::set<uint32_t> seen;
    while (!stack.empty()) {
      const uint32_t off = stack.back();
      stack.pop_back();
      if (!seen.insert(off).second || off + 14 > data.size()) continue;
      if (++entries > kMaxEntries) return L"The disc image is damaged (too many files).";
      const uint8_t* entry = data.data() + off;
      const uint16_t left = Le16(entry);
      const uint16_t right = Le16(entry + 2);
      if (left == 0xFFFF) continue;  // sector padding
      const uint32_t start = Le32(entry + 4);
      const uint32_t length = Le32(entry + 8);
      const uint8_t attributes = entry[12];
      const uint8_t name_length = entry[13];
      if (off + 14 + name_length > data.size()) continue;
      const std::string name(reinterpret_cast<const char*>(entry + 14), name_length);
      if (!SafeName(name)) return L"The disc image contains an unexpected file name.";
      if (attributes & kAttrDirectory) {
        directories.push_back({start, length, dir.prefix + name + "/"});
      } else {
        const uint64_t offset = base + uint64_t(start) * kSector;
        if (offset + length > image_size) {
          return L"The disc image is incomplete (a file goes past its end). Copy it again.";
        }
        files.push_back({dir.prefix + name, offset, length});
      }
      if (left) stack.push_back(uint32_t(left) * 4);
      if (right) stack.push_back(uint32_t(right) * 4);
    }
  }
  return {};
}

}  // namespace

// --- SHA-256 ---------------------------------------------------------------

namespace {
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
         0x5be0cd19} {}

void Sha256::Block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = Be32(p + i * 4);
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                        kK[i] + w[i];
    const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
  h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::Update(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  length_ += size;
  if (buffered_) {
    const size_t take = std::min(size, 64 - buffered_);
    std::memcpy(buffer_ + buffered_, p, take);
    buffered_ += take;
    p += take;
    size -= take;
    if (buffered_ < 64) return;
    Block(buffer_);
    buffered_ = 0;
  }
  for (; size >= 64; p += 64, size -= 64) Block(p);
  std::memcpy(buffer_, p, size);
  buffered_ = size;
}

std::string Sha256::HexDigest() {
  const uint64_t bits = length_ * 8;
  uint8_t pad[72] = {0x80};
  const size_t pad_size = (buffered_ < 56 ? 56 : 120) - buffered_;
  Update(pad, pad_size);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
  Update(len, 8);
  std::string hex;
  char part[9];
  for (uint32_t word : h_) {
    snprintf(part, sizeof(part), "%08x", word);
    hex += part;
  }
  return hex;
}

// --- public ------------------------------------------------------------------

std::wstring SizeText(uint64_t bytes) {
  wchar_t text[64];
  if (bytes >= (1ull << 30)) {
    swprintf(text, 64, L"%.1f GB", double(bytes) / double(1ull << 30));
  } else {
    swprintf(text, 64, L"%llu MB", (unsigned long long)((bytes + (1ull << 20) - 1) >> 20));
  }
  return text;
}

namespace {

bool EndsWith(const std::wstring& text, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  return text.size() >= n && _wcsicmp(text.c_str() + text.size() - n, suffix) == 0;
}

// Lists the files below `dir` (relative, '/' separated). False when the
// folder holds links, odd names or too many files.
bool ListFolder(const std::wstring& root, const std::string& prefix, std::vector<DiscFile>& files,
                std::wstring& error) {
  WIN32_FIND_DATAW data;
  const std::wstring dir = prefix.empty() ? root : Join(root, Widen(prefix));
  HANDLE find = FindFirstFileW(Join(dir, L"*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return true;
  bool ok = true;
  do {
    const std::wstring wname = data.cFileName;
    if (wname == L"." || wname == L"..") continue;
    const std::string name = Narrow(wname);
    if (!SafeName(name) || Widen(name) != wname) {
      error = L"The game folder contains an unexpected file name: " + Join(dir, wname);
      ok = false;
      break;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      error = L"The game folder contains links. Select ordinary extracted files.";
      ok = false;
      break;
    }
    const std::string path = prefix + name;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (Lower(path + "/").rfind(kSkippedDir, 0) == 0) continue;
      if (!ListFolder(root, path + "/", files, error)) {
        ok = false;
        break;
      }
    } else {
      if (files.size() >= kMaxEntries) {
        error = L"The game folder holds too many files.";
        ok = false;
        break;
      }
      DiscFile file;
      file.path = path;
      file.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
      file.source = Join(root, Widen(path));
      files.push_back(std::move(file));
    }
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return ok;
}

std::wstring CheckFolder(const std::wstring& folder, Disc& disc) {
  const std::wstring xex = Join(folder, L"default.xex");
  if (!IsFile(xex) || !IsDir(Join(folder, L"data"))) {
    return L"This folder has no default.xex and data folder. Select the extracted game folder "
           L"that contains both, or the disc image.";
  }
  if (std::wstring error = CheckXexFile(xex); !error.empty()) return error;
  std::vector<DiscFile> all;
  std::wstring error;
  if (!ListFolder(folder, "", all, error)) return error;
  disc.image = folder;
  disc.folder = true;
  DiscFile xex_file;
  for (DiscFile& file : all) {
    if (Lower(file.path) == "default.xex") {
      xex_file = std::move(file);
      continue;
    }
    disc.total_bytes += file.size;
    disc.files.push_back(std::move(file));
  }
  disc.total_bytes += xex_file.size;
  disc.files.push_back(std::move(xex_file));
  return {};
}

}  // namespace

std::wstring CheckDisc(const std::wstring& source, Disc& disc) {
  disc = {};
  if (IsDir(source)) return CheckFolder(FullPath(source), disc);
  if (EndsWith(source, L"\\default.xex") || EndsWith(source, L"/default.xex") ||
      _wcsicmp(source.c_str(), L"default.xex") == 0) {
    return CheckFolder(DirName(FullPath(source)), disc);
  }
  for (const wchar_t* archive : {L".rar", L".zip", L".7z", L".gz", L".xz"}) {
    if (EndsWith(source, archive)) {
      return L"This is an archive, not a disc image. Extract it first (for example with "
             L"7-Zip), then select the .iso, or the extracted folder with default.xex.";
    }
  }
  const std::wstring& image = source;
  File in;
  if (!in.OpenRead(image)) return L"The file could not be opened.";
  const uint64_t image_size = in.Size();
  std::vector<DiscFile> all;
  if (std::wstring error = ReadFileTable(in, image_size, all); !error.empty()) return error;
  const DiscFile* xex = nullptr;
  for (const DiscFile& file : all) {
    if (Lower(file.path) == "default.xex") xex = &file;
  }
  if (!xex) return L"This disc image has no default.xex.";
  if (xex->size > kMaxXexSize) return L"default.xex on this disc is not an Xbox 360 executable.";
  std::vector<uint8_t> data(size_t(xex->size));
  if (!in.ReadAt(xex->offset, data.data(), data.size())) return L"The disc image could not be read.";
  if (std::wstring error = CheckXex(data); !error.empty()) return error;

  disc.image = image;
  const DiscFile xex_file = *xex;
  for (DiscFile& file : all) {
    const std::string lower = Lower(file.path);
    if (lower.rfind(kSkippedDir, 0) == 0 || lower == "default.xex") continue;
    disc.total_bytes += file.size;
    disc.files.push_back(std::move(file));
  }
  // Disc order reads the image front to back; default.xex goes last, so a
  // folder only looks installed once everything else is there.
  std::sort(disc.files.begin(), disc.files.end(),
            [](const DiscFile& a, const DiscFile& b) { return a.offset < b.offset; });
  disc.files.push_back(xex_file);
  disc.total_bytes += xex_file.size;
  return {};
}

std::wstring CheckXexFile(const std::wstring& path) {
  File in;
  if (!in.OpenRead(path)) return L"default.xex could not be opened.";
  const uint64_t size = in.Size();
  if (size > kMaxXexSize) return L"default.xex is not an Xbox 360 executable.";
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (!in.ReadAt(0, data.data(), data.size())) return L"default.xex could not be read.";
  return CheckXex(data);
}

}  // namespace skate2
