#pragma once

#include <cstdio>
#include <cstdarg>
#include <mutex>

#include "config.h"

namespace dicker {

struct Logger {
  const Config* cfg = nullptr;
  std::mutex mtx;

  void configure(const Config& config) {
    this->cfg = &config;
  }

  bool verbose() const {
    return this->cfg != nullptr && this->cfg->verbose;
  }

  void info(const char* fmt, ...) {
    if (!this->verbose()) {
      return;
    }
    va_list ap;
    va_start(ap, fmt);
    this->emit("[dicker] ", fmt, ap);
    va_end(ap);
  }

  void error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    this->emit("[!] ", fmt, ap);
    va_end(ap);
  }

  void emit(const char* prefix, const char* fmt, va_list ap) {
    std::lock_guard<std::mutex> lk(this->mtx);
    std::fputs(prefix, stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
  }
};

inline Logger& log() {
  static Logger instance;
  return instance;
}

}  // namespace dicker
