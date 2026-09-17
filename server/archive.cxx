#include "archive.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dicker {

  namespace {

    constexpr std::size_t kRawTarget = 1u << 18;  // 256 KiB of tar bytes per step

    // Write `v` as (n-1) octal digits, zero-padded, followed by a NUL.
    void set_octal(std::uint8_t* dst, std::size_t n, std::uint64_t v) {
      for (std::size_t k = 0; k + 1 < n; ++k) {
        dst[n - 2 - k] = static_cast<std::uint8_t>('0' + (v & 7u));
        v >>= 3;
      }
      dst[n - 1] = '\0';
    }

  }  // namespace

  ArchiveStreamer::~ArchiveStreamer() {
    if (zcs_ != nullptr) {
      ZSTD_freeCStream(zcs_);
    }
  }

  bool ArchiveStreamer::begin(const std::filesystem::path& dir,
                              const std::string& prefix, int level) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      return false;
    }

    std::filesystem::recursive_directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code fec;
      if (!it->is_regular_file(fec) || fec) {
        continue;
      }
      std::filesystem::path rel = std::filesystem::relative(it->path(), dir, fec);
      if (fec || rel.empty()) {
        continue;
      }
      std::uint64_t size = static_cast<std::uint64_t>(
          std::filesystem::file_size(it->path(), fec));
      if (fec) {
        continue;
      }
      Entry entry;
      entry.path = it->path();
      entry.name = prefix + "/" + rel.generic_string();
      entry.size = size;
      entries_.push_back(std::move(entry));
    }

    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });

    zcs_ = ZSTD_createCStream();
    if (zcs_ == nullptr) {
      return false;
    }
    ZSTD_CCtx_setParameter(zcs_, ZSTD_c_compressionLevel, level);

    idx_ = 0;
    header_pending_ = true;
    return true;
  }

  void ArchiveStreamer::emit_header(const std::string& name, std::uint64_t size) {
    std::uint8_t h[512];
    std::memset(h, 0, sizeof h);

    // ustar splits long names into name[100] + prefix[155] at a '/' boundary.
    std::string field_name = name;
    std::string field_prefix;
    if (name.size() > 100) {
      std::size_t split = std::string::npos;
      for (std::size_t p = 0; p < name.size(); ++p) {
        if (name[p] == '/') {
          std::size_t suffix_len = name.size() - (p + 1);
          if (suffix_len <= 100 && p <= 155) {
            split = p;  // prefer the largest acceptable prefix
          }
        }
      }
      if (split != std::string::npos) {
        field_prefix = name.substr(0, split);
        field_name = name.substr(split + 1);
      }
      else {
        field_name = name.substr(name.size() - 100);  // best effort
      }
    }

    std::memcpy(h + 0, field_name.data(),
                std::min<std::size_t>(field_name.size(), 100));
    std::memcpy(h + 345, field_prefix.data(),
                std::min<std::size_t>(field_prefix.size(), 155));

    set_octal(h + 100, 8, 0644);   // mode
    set_octal(h + 108, 8, 0);      // uid
    set_octal(h + 116, 8, 0);      // gid
    set_octal(h + 124, 12, size);  // size
    set_octal(h + 136, 12, 0);     // mtime (deterministic)
    h[156] = '0';                  // typeflag: regular file
    std::memcpy(h + 257, "ustar", 5);  // magic (NUL-terminated by memset)
    h[263] = '0';                  // version "00"
    h[264] = '0';

    std::memset(h + 148, ' ', 8);  // checksum field spaces during computation
    unsigned sum = 0;
    for (int k = 0; k < 512; ++k) {
      sum += h[k];
    }
    char cs[8];
    std::snprintf(cs, sizeof cs, "%06o", sum & 0x1FFFFFu);
    std::memcpy(h + 148, cs, 6);
    h[154] = '\0';
    h[155] = ' ';

    raw_.insert(raw_.end(), h, h + 512);
  }

  void ArchiveStreamer::refill_raw() {
    std::uint8_t buf[65536];

    while ((raw_.size() - raw_pos_) < kRawTarget && !input_done_) {
      if (idx_ >= entries_.size()) {
        if (!trailer_emitted_) {
          raw_.insert(raw_.end(), 1024, 0);  // two zero blocks terminate the tar
          trailer_emitted_ = true;
        }
        input_done_ = true;
        break;
      }

      Entry& entry = entries_[idx_];

      if (header_pending_) {
        cur_.close();
        cur_.clear();
        cur_.open(entry.path, std::ios::binary);
        if (!cur_) {
          ++idx_;  // unreadable now; skip it
          header_pending_ = true;
          continue;
        }
        emit_header(entry.name, entry.size);
        cur_remaining_ = entry.size;
        pad_remaining_ =
            (512 - static_cast<std::size_t>(entry.size % 512)) % 512;
        header_pending_ = false;
        continue;
      }

      if (cur_remaining_ > 0) {
        std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(cur_remaining_, sizeof buf));
        cur_.read(reinterpret_cast<char*>(buf),
                  static_cast<std::streamsize>(want));
        std::streamsize got = cur_.gcount();
        if (got > 0) {
          raw_.insert(raw_.end(), buf, buf + got);
          cur_remaining_ -= static_cast<std::uint64_t>(got);
        }
        if (got < static_cast<std::streamsize>(want)) {
          // File shrank mid-read: zero-fill the declared remainder so the
          // member length still matches the header.
          raw_.insert(raw_.end(), static_cast<std::size_t>(cur_remaining_), 0);
          cur_remaining_ = 0;
        }
        continue;
      }

      if (pad_remaining_ > 0) {
        raw_.insert(raw_.end(), pad_remaining_, 0);
        pad_remaining_ = 0;
        continue;
      }

      cur_.close();
      ++idx_;
      header_pending_ = true;
    }
  }

  bool ArchiveStreamer::next(std::vector<std::uint8_t>& out) {
    out.clear();
    if (!ok_ || finished_) {
      return false;
    }

    std::uint8_t scratch[65536];

    while (out.empty() && !finished_) {
      refill_raw();

      std::size_t avail = raw_.size() - raw_pos_;
      bool ending = input_done_ && avail == 0;
      ZSTD_inBuffer in{raw_.data() + raw_pos_, avail, 0};
      ZSTD_EndDirective mode = ending ? ZSTD_e_end : ZSTD_e_continue;

      while (true) {
        ZSTD_outBuffer ob{scratch, sizeof scratch, 0};
        std::size_t rc = ZSTD_compressStream2(zcs_, &ob, &in, mode);
        if (ZSTD_isError(rc)) {
          ok_ = false;
          return false;
        }
        out.insert(out.end(), scratch, scratch + ob.pos);
        if (ending) {
          if (rc == 0) {
            finished_ = true;
            break;
          }
          continue;  // keep flushing the tail
        }
        if (in.pos == in.size) {
          break;  // all provided input consumed
        }
      }

      raw_pos_ += in.pos;
      if (raw_pos_ == raw_.size()) {
        raw_.clear();
        raw_pos_ = 0;
      }
    }

    return !finished_;
  }

}
