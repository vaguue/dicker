#pragma once

#include <cstring>
#include <cstdint>
#include <cstddef>
#include <vector>

#include "protocol.h"
#include "chan.h"

const size_t MAX_PATH = 256;

struct Task {
  dicker::UnitType type;
  char pathname[MAX_PATH];
  size_t offset, length;
};

struct WorkerTask : Task {
  Completion* completion = nullptr;

  WorkerTask() = default;

  WorkerTask(
    dicker::UnitType type,
    const char* path,
    size_t offset,
    size_t length
  ) : Task{type, {}, offset, length}
  {
    std::strncpy(this->pathname, path, MAX_PATH - 1);
    this->pathname[MAX_PATH - 1] = '\0';
  }
};

// A whole unit to read (file, or a chunk's byte range). The reader streams it
// back through its own buffer ring, so no buffer or count travels on the task.
struct ReadRequest : Task {
  Completion* completion = nullptr;
};

// Owns its payload: the network sends on its own thread, so the bytes must
// outlive the caller's buffer. One heap copy per packet — deliberately simple.
struct NetworkTask {
  std::vector<uint8_t> data;
  Completion* completion = nullptr;

  NetworkTask() = default;

  NetworkTask(const uint8_t* bytes, size_t len) : data(bytes, bytes + len) {
  }
};
