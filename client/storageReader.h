#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <filesystem>

#include "queue.h"
#include "task.h"

namespace fs = std::filesystem;

struct StorageReader : Queue<Task, StorageReader> {
  std::size_t read_range(std::uint8_t* buffer, const char* pathname,
                         std::size_t offset, std::size_t len) {
    FILE* file = std::fopen(pathname, "rb");
    if (file == nullptr) {
      return 0;
    }
    if (offset != 0) {
      std::fseek(file, static_cast<long>(offset), SEEK_SET);
    }
    std::size_t got = std::fread(buffer, 1, len, file);
    std::fclose(file);
    return got;
  }

  void process(const Task&) {
  }
};
