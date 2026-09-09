#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <filesystem>

#include "chan.h"
#include "task.h"

namespace fs = std::filesystem;

struct StorageReader : Chan<StorageTask, StorageReader, 64> {
  void process(StorageTask& t) {
    FILE* file = std::fopen(t.pathname, "rb");
    if (file == nullptr) {
      return;
    }
    if (t.offset != 0) {
      std::fseek(file, static_cast<long>(t.offset), SEEK_SET);
    }

    std::size_t got = std::fread(t.buf, 1, t.length, file);

    std::fclose(file);
    t.got = got;
  }
};
