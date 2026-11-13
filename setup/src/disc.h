// SPDX-License-Identifier: MIT
// Reads the player's Xbox 360 Skate 2 disc image (XDVDFS) and checks that
// its default.xex is the revision the bundled skate2.exe was compiled from.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skate2 {

inline constexpr uint32_t kTitleId = 0x4541087F;
inline constexpr uint32_t kMediaId = 0x6ADB5821;
inline constexpr char kXexSha256[] =
    "116ba3ee37fcf0d8db60b7d47c971fdb6d3eb2a7a4d3cfc3611a06704a588ae5";

// Streaming SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  Sha256();
  void Update(const void* data, size_t size);
  std::string HexDigest();  // finishes the hash

 private:
  void Block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buffer_[64];
  size_t buffered_ = 0;
  uint64_t length_ = 0;
};

struct DiscFile {
  std::string path;     // as on the disc, '/' separated: "data/audio/x.big"
  uint64_t offset = 0;  // absolute byte offset in the image
  uint64_t size = 0;
  std::wstring source;  // extracted folder: the file to copy; image: empty
};

struct Disc {
  std::wstring image;   // the disc image, or the extracted game folder
  bool folder = false;  // true: files come from an extracted folder
  std::vector<DiscFile> files;  // disc order, default.xex last
  uint64_t total_bytes = 0;
};

// Checks a game source and fills `disc`: a disc image (XDVDFS file table),
// an extracted game folder holding default.xex and data\, or that
// default.xex itself. default.xex must match (title ID and SHA-256).
// Returns an empty string, or a message for the player.
std::wstring CheckDisc(const std::wstring& source, Disc& disc);

// The same check for an already extracted default.xex.
std::wstring CheckXexFile(const std::wstring& path);

std::wstring SizeText(uint64_t bytes);

}  // namespace skate2
