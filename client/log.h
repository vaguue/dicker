#pragma once

#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <mutex>

#include "config.h"

namespace dicker {

struct Logger {
  std::atomic<bool> enabled{false};
  std::mutex mtx;

  void configure(bool on) {
    this->enabled.store(on, std::memory_order_relaxed);
  }

  void configure(const Config& config) {
    this->configure(config.verbose);
  }

  bool verbose() const {
    return false;
    //return this->enabled.load(std::memory_order_relaxed);
  }

  void info(const char* fmt, ...) {
    if (!this->verbose()) {
      return;
    }

    va_list ap;
    va_start(ap, fmt);
    this->emit("[*] ", fmt, ap);
    va_end(ap);
  }

  void error(const char* fmt, ...) {
    if (!this->verbose()) {
      return;
    }

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
