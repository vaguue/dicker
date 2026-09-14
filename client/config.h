#pragma once

#include <cstddef>

#include "conn.h"

namespace dicker {

struct Config {
  Conn conn;

  size_t diskConcurrency = 1;
  size_t workers = 4;
  size_t bigFileThreshold = 256 * 1024 * 1024;
  size_t chunkSize = 64 * 1024 * 1024;
  bool verbose = false;
};

}  // namespace dicker
