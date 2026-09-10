#pragma once

#include <cstdint>
#include <cstddef>
#include <algorithm>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

#include "chan.h"
#include "ring.h"
#include "task.h"
#include "storage.h"

static constexpr size_t READ_BLOCK = 256 * 1024;
static constexpr size_t READ_AHEAD = 4;
static constexpr size_t READ_RING = 8;

struct Filled {
  uint32_t idx = 0;
  uint32_t len = 0;
  bool eof = false;
};

struct ReadFileHandle {
#ifdef _WIN32
  HANDLE h = INVALID_HANDLE_VALUE;

  bool open(const char* utf8) {
    std::wstring w = Storage::toWide(utf8);
    this->h = CreateFileW(w.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    return this->h != INVALID_HANDLE_VALUE;
  }

  size_t readAt(uint8_t* buf, size_t len, uint64_t off) {
    size_t total = 0;
    while (total < len) {
      const uint64_t pos = off + total;
      OVERLAPPED ov{};
      ov.Offset = static_cast<DWORD>(pos & 0xffffffffu);
      ov.OffsetHigh = static_cast<DWORD>(pos >> 32);

      DWORD want = static_cast<DWORD>(std::min<size_t>(len - total, 1u << 30));
      DWORD got = 0;
      if (!ReadFile(this->h, buf + total, want, &got, &ov) || got == 0) {
        break;
      }
      total += got;
    }
    return total;
  }

  void close() {
    if (this->h != INVALID_HANDLE_VALUE) {
      CloseHandle(this->h);
      this->h = INVALID_HANDLE_VALUE;
    }
  }
#else
  int fd = -1;

  bool open(const char* path) {
    this->fd = ::open(path, O_RDONLY);
    return this->fd >= 0;
  }

  size_t readAt(uint8_t* buf, size_t len, uint64_t off) {
    size_t total = 0;
    while (total < len) {
      ssize_t got = ::pread(this->fd, buf + total, len - total,
                            static_cast<off_t>(off + total));
      if (got <= 0) {
        break;
      }
      total += static_cast<size_t>(got);
    }
    return total;
  }

  void close() {
    if (this->fd >= 0) {
      ::close(this->fd);
      this->fd = -1;
    }
  }
#endif
};

struct StorageReader : Chan<StorageReader, ReadRequest, 8> {
  Storage* storage;

  uint8_t bufs[READ_AHEAD][READ_BLOCK];
  SpscRing<uint32_t, READ_RING> spare;
  SpscRing<Filled, READ_RING> ready;

  explicit StorageReader(Storage* storage) : storage{storage} {
    for (uint32_t i = 0; i < READ_AHEAD; i += 1) {
      this->spare.push(i);
    }
  }

  ~StorageReader() {
    this->spare.close();
    this->ready.close();
  }

  void process(ReadRequest& req) {
    std::string resolved = this->storage->resolve(req.pathname);

    this->storage->diskLimit.acquire();

    ReadFileHandle file;
    bool opened = file.open(resolved.c_str());

    uint64_t off = req.offset;
    uint64_t remaining = req.length;

    while (opened && remaining > 0) {
      uint32_t idx;
      if (!this->spare.pop(idx)) {
        break;
      }

      size_t want = remaining < READ_BLOCK ? static_cast<size_t>(remaining) : READ_BLOCK;
      size_t got = file.readAt(this->bufs[idx], want, off);
      if (got == 0) {
        this->spare.push(idx);
        break;
      }

      this->ready.push(Filled{idx, static_cast<uint32_t>(got), false});
      off += got;
      remaining -= got;
    }

    file.close();
    this->storage->diskLimit.release();

    this->ready.push(Filled{0, 0, true});
  }

  void submit(dicker::UnitType type, const char* path, uint64_t offset, uint64_t length) {
    ReadRequest req;
    req.type = type;
    std::strncpy(req.pathname, path, PATH_CAP - 1);
    req.pathname[PATH_CAP - 1] = '\0';
    req.offset = offset;
    req.length = length;
    this->enqueue(req);
  }

  bool next(Filled& out) {
    return this->ready.pop(out);
  }

  void recycle(uint32_t idx) {
    this->spare.push(idx);
  }

  uint8_t* data(uint32_t idx) {
    return this->bufs[idx];
  }
};
