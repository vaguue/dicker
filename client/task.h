#pragma once

#include "protocol.hpp"

using AffinityKey = uint32_t;
constexpr AffinityKey NONE = 0;

const size_t MAX_PATH = 256;

struct Task {
  dicker::UnitType type;
  char pathname[MAX_PATH];
  size_t offset, length;
};
