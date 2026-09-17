#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zstd.h>

namespace dicker {

  // Streams a directory as a zstd-compressed tar archive (.tar.zst) in bounded
  // chunks. Fully synchronous and single-threaded: the caller pulls one
  // compressed chunk at a time with next() and writes it to the socket, so peak
  // memory stays bounded regardless of the archive's total size.
  struct ArchiveStreamer {
    ArchiveStreamer() = default;
    ~ArchiveStreamer();

    ArchiveStreamer(const ArchiveStreamer&) = delete;
    ArchiveStreamer& operator=(const ArchiveStreamer&) = delete;

    // Collect the regular files under `dir`, naming archive members
    // "<prefix>/<path-relative-to-dir>". `level` is the zstd compression level.
    bool begin(const std::filesystem::path& dir, const std::string& prefix, int level);

    // Append the next compressed chunk to `out` (cleared first). Returns true if
    // more chunks remain (call again); false once the whole archive is emitted.
    // When it returns true, `out` is non-empty.
    bool next(std::vector<std::uint8_t>& out);

    bool ok() const { return ok_; }

  private:
    struct Entry {
      std::filesystem::path path;
      std::string name;
      std::uint64_t size = 0;
    };

    void refill_raw();  // extend raw_ with more uncompressed tar bytes
    void emit_header(const std::string& name, std::uint64_t size);

    std::vector<Entry> entries_;
    std::size_t idx_ = 0;
    bool header_pending_ = true;

    std::ifstream cur_;
    std::uint64_t cur_remaining_ = 0;   // file data bytes still to read
    std::size_t pad_remaining_ = 0;     // tar padding bytes still to emit
    bool trailer_emitted_ = false;

    std::vector<std::uint8_t> raw_;     // staged uncompressed tar bytes
    std::size_t raw_pos_ = 0;
    bool input_done_ = false;           // all tar bytes (incl trailer) produced
    bool finished_ = false;             // zstd stream fully flushed
    bool ok_ = true;

    ZSTD_CStream* zcs_ = nullptr;
  };

}
