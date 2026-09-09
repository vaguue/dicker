#pragma once

#include <array>
#include <string>
#include <cstdint>

#include "net.h"

struct Conn {
  const char* host;
  const char* port;
  net::Proxy proxy;
  std::array<uint8_t, 16> sessionId;
  std::string key;
  uint8_t flags;
};
