#pragma once

#include <cstring>
#include <cstdint>
#include <cstddef>
#include <vector>

#include "protocol.h"
#include "muypoco/chan.h"

namespace dicker {

const size_t PATH_CAP = 256;

struct Task {
  dicker::UnitType type;
  char pathname[PATH_CAP];
  size_t offset, length;
  uint64_t fileSize;
};

struct WorkerTask : Task {
  std::shared_ptr<muypoco::Completion> completion;

  WorkerTask() = default;

  WorkerTask(
    dicker::UnitType type,
    const char* path,
    size_t offset,
    size_t length,
    uint64_t fileSize
  ) : Task{type, {}, offset, length, fileSize}
  {
    std::strncpy(this->pathname, path, PATH_CAP - 1);
    this->pathname[PATH_CAP - 1] = '\0';
  }
};

struct ReadRequest : Task {
  std::shared_ptr<muypoco::Completion> completion;
};

struct NetworkTask {
  std::vector<uint8_t> data; //TODO no zero-alloc ;(
  std::shared_ptr<muypoco::Completion> completion;

  NetworkTask() = default;

  NetworkTask(const uint8_t* bytes, size_t len) : data(bytes, bytes + len) {
  }
};

struct UploaderTask {
  std::string fn;
  std::shared_ptr<muypoco::Completion> completion;
};

}  // namespace dicker
