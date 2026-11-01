#pragma once

#include <cstdint>
#include <string_view>

#include "muypoco/events.h"

#include "protocol.h"

namespace dicker {

MUYPOCO_BUS(events);

struct UploadStarted {
  std::string_view path;
  UnitType type;
  uint64_t offset;
  uint64_t length;
  uint64_t fileSize;
};

struct UploadFinished {
  std::string_view path;
  UnitType type;
  uint64_t offset;
  uint64_t length;
  uint64_t fileSize;
  uint64_t compressedBytes;
  bool ok;
};

}  // namespace dicker
