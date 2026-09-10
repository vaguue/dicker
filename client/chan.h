#pragma once

#include <array>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <utility>

using AffinityKey = uint32_t;
constexpr AffinityKey NONE = 0;

struct Completion {
  std::atomic<bool> done{false};

  void wait() {
    this->done.wait(false, std::memory_order_acquire);
  }

  void signal() {
    this->done.store(true, std::memory_order_release);
    this->done.notify_one();
  }
};

template<typename Impl, typename Task, size_t Cap>
struct Chan {
  static constexpr size_t Mask = Cap - 1;

  std::array<Task, Cap> buf;

  alignas(64) std::atomic<size_t> head{0};
  alignas(64) std::atomic<size_t> tail{0};

  alignas(64) std::atomic<uint32_t> notEmptyGate{0};
  alignas(64) std::atomic<uint32_t> notFullGate{0};
  alignas(64) std::atomic<bool> stopped{false};

  std::thread th;

  std::atomic<AffinityKey> warm{NONE};

  Chan() = default;
  Chan(const Chan&) = delete;
  Chan& operator=(const Chan&) = delete;

  void start() {
    this->th = std::thread([this]{ this->run(); });
  }

  uint32_t occupancy() const {
    return static_cast<uint32_t>(this->tail.load(std::memory_order_relaxed) -
                                 this->head.load(std::memory_order_relaxed));
  }

  AffinityKey warmKey() const {
    return this->warm.load(std::memory_order_relaxed);
  }

  void setWarm(AffinityKey key) {
    this->warm.store(key, std::memory_order_relaxed);
  }

  void publish(size_t tl, Task&& t) {
    this->buf[tl & Mask] = std::move(t);
    this->tail.store(tl + 1, std::memory_order_release);
    this->notEmptyGate.fetch_add(1, std::memory_order_release);
    this->notEmptyGate.notify_one();
  }

  bool tryEnqueue(Task t) {
    if (this->stopped.load(std::memory_order_acquire)) {
      return false;
    }

    const size_t tl = this->tail.load(std::memory_order_relaxed);
    if (tl - this->head.load(std::memory_order_acquire) >= Cap) {
      return false;
    }

    this->publish(tl, std::move(t));
    return true;
  }

  bool enqueue(Task t) {
    const size_t tl = this->tail.load(std::memory_order_relaxed);

    for (;;) {
      if (this->stopped.load(std::memory_order_acquire)) {
        return false;
      }
      if (tl - this->head.load(std::memory_order_acquire) < Cap) {
        break;
      }

      const uint32_t g = this->notFullGate.load(std::memory_order_acquire);
      if (tl - this->head.load(std::memory_order_acquire) < Cap ||
          this->stopped.load(std::memory_order_acquire)) {
        continue;
      }
      this->notFullGate.wait(g, std::memory_order_acquire);
    }

    this->publish(tl, std::move(t));
    return true;
  }

  bool await(Task t) {
    Completion c;
    t.completion = &c;

    if (!this->enqueue(std::move(t))) {
      return false;
    }

    c.wait();
    return true;
  }

  void run() {
    if constexpr (requires(Impl* self) { self->init(); }) {
      static_cast<Impl*>(this)->init();
    }

    size_t hd = this->head.load(std::memory_order_relaxed);

    for (;;) {
      if (hd == this->tail.load(std::memory_order_acquire)) {
        if (this->stopped.load(std::memory_order_acquire)) {
          if (hd == this->tail.load(std::memory_order_acquire)) {
            return;
          }
          continue;
        }

        const uint32_t g = this->notEmptyGate.load(std::memory_order_acquire);
        if (hd != this->tail.load(std::memory_order_acquire) ||
            this->stopped.load(std::memory_order_acquire)) {
          continue;
        }
        this->notEmptyGate.wait(g, std::memory_order_acquire);
        continue;
      }

      Task t = std::move(this->buf[hd & Mask]);
      hd += 1;
      this->head.store(hd, std::memory_order_release);
      this->notFullGate.fetch_add(1, std::memory_order_release);
      this->notFullGate.notify_one();

      static_cast<Impl*>(this)->process(t);

      if (t.completion != nullptr) {
        t.completion->signal();
      }
    }
  }

  ~Chan() {
    this->stopped.store(true, std::memory_order_release);

    this->notEmptyGate.fetch_add(1, std::memory_order_release);
    this->notEmptyGate.notify_all();
    this->notFullGate.fetch_add(1, std::memory_order_release);
    this->notFullGate.notify_all();

    if (this->th.joinable()) {
      this->th.join();
    }
  }
};
