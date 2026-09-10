#pragma once

#include <cstring>
#include <cstdint>
#include <cstddef>
#include <vector>

#include "protocol.h"
#include "chan.h"

const size_t PATH_CAP = 256;

struct Task {
  dicker::UnitType type;
  char pathname[PATH_CAP];
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
    std::strncpy(this->pathname, path, PATH_CAP - 1);
    this->pathname[PATH_CAP - 1] = '\0';
  }
};

struct ReadRequest : Task {
  Completion* completion = nullptr;
};

struct NetworkTask {
  std::vector<uint8_t> data; //TODO no zero-alloc ;(
  Completion* completion = nullptr;

  NetworkTask() = default;

  NetworkTask(const uint8_t* bytes, size_t len) : data(bytes, bytes + len) {
  }
};
