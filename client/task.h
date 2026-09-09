#pragma once

#include "protocol.hpp"
#include "chan.h"

const size_t MAX_PATH = 256;

struct Task {
  dicker::UnitType type;
  char pathname[MAX_PATH];
  size_t offset, length;
};

struct WorkerTask : Task {
  Completion* completion = nullptr;
};

struct StorageTask : Task {
  uint8_t* buf;
  size_t got = 0;
  Completion* completion = nullptr;

  StorageTask(
    dicker::UnitType type,
    const char* path,
    size_t offset,
    size_t length,
    uint8_t* buf,
    Completion* completion = nullptr
  ) : Task{type, {}, offset, length}, buf(buf), got(0), completion(completion)
  {
    std::strncpy(this->pathname, path, MAX_PATH - 1);
    this->pathname[MAX_PATH - 1] = '\0';
  }
};

struct NetworkTask {
  const uint8_t* data;
  size_t length;
  Completion* completion = nullptr;
};
