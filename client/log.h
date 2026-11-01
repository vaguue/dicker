#pragma once

#include "muypoco/log.h"

#include "config.h"

namespace dicker {

struct Logger : muypoco::Logger {
  using muypoco::Logger::configure;

  Logger() {
    this->configure(muypoco::level::off);
  }

  void configure(const Config& config) {
    this->configure(config.verbose ? muypoco::level::info : muypoco::level::off);
  }
};

inline Logger& log() {
  static Logger instance;
  return instance;
}

}  // namespace dicker
