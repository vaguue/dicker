#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <utility>

template<typename T, size_t Cap>
struct SpscRing {
  static constexpr size_t Mask = Cap - 1;

  std::array<T, Cap> buf;

  alignas(64) std::atomic<size_t> head{0};
  alignas(64) std::atomic<size_t> tail{0};

  alignas(64) std::atomic<uint32_t> notEmptyGate{0};
  alignas(64) std::atomic<uint32_t> notFullGate{0};
  alignas(64) std::atomic<bool> closed{false};

  void close() {
    this->closed.store(true, std::memory_order_release);
    this->notEmptyGate.fetch_add(1, std::memory_order_release);
    this->notEmptyGate.notify_all();
    this->notFullGate.fetch_add(1, std::memory_order_release);
    this->notFullGate.notify_all();
  }

  bool push(T v) {
    const size_t tl = this->tail.load(std::memory_order_relaxed);

    for (;;) {
      if (this->closed.load(std::memory_order_acquire)) {
        return false;
      }
      if (tl - this->head.load(std::memory_order_acquire) < Cap) {
        break;
      }

      const uint32_t g = this->notFullGate.load(std::memory_order_acquire);
      if (tl - this->head.load(std::memory_order_acquire) < Cap ||
          this->closed.load(std::memory_order_acquire)) {
        continue;
      }
      this->notFullGate.wait(g, std::memory_order_acquire);
    }

    this->buf[tl & Mask] = std::move(v);
    this->tail.store(tl + 1, std::memory_order_release);
    this->notEmptyGate.fetch_add(1, std::memory_order_release);
    this->notEmptyGate.notify_one();
    return true;
  }

  bool pop(T& out) {
    const size_t hd = this->head.load(std::memory_order_relaxed);

    for (;;) {
      if (hd != this->tail.load(std::memory_order_acquire)) {
        break;
      }
      if (this->closed.load(std::memory_order_acquire)) {
        if (hd == this->tail.load(std::memory_order_acquire)) {
          return false;
        }
        break;
      }

      const uint32_t g = this->notEmptyGate.load(std::memory_order_acquire);
      if (hd != this->tail.load(std::memory_order_acquire) ||
          this->closed.load(std::memory_order_acquire)) {
        continue;
      }
      this->notEmptyGate.wait(g, std::memory_order_acquire);
    }

    out = std::move(this->buf[hd & Mask]);
    this->head.store(hd + 1, std::memory_order_release);
    this->notFullGate.fetch_add(1, std::memory_order_release);
    this->notFullGate.notify_one();
    return true;
  }
};
