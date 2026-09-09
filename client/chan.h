#pragma once

#include <array>
#include <thread>
#include <mutex>
#include <condition_variable>
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
  std::array<Task, Cap> buf;

  size_t head = 0;
  size_t tail = 0;
  size_t count = 0;

  std::mutex m;

  std::condition_variable notEmpty;
  std::condition_variable notFull;

  std::thread th;

  bool stop = false;

  std::atomic<uint32_t> load{0};
  std::atomic<AffinityKey> warm{NONE};

  Chan() = default;
  Chan(const Chan&) = delete;
  Chan& operator=(const Chan&) = delete;

  void start() {
    this->th = std::thread([this]{ this->run(); });
  }

  uint32_t occupancy() const {
    return this->load.load(std::memory_order_relaxed);
  }

  AffinityKey warmKey() const {
    return this->warm.load(std::memory_order_relaxed);
  }

  void setWarm(AffinityKey key) {
    this->warm.store(key, std::memory_order_relaxed);
  }

  bool tryEnqueue(Task t) {
    {
      std::lock_guard lk(this->m);

      if (this->stop || this->count == Cap) {
        return false;
      }

      this->buf[this->tail] = std::move(t);
      this->tail = (this->tail + 1) % Cap;
      this->count += 1;
    }

    this->load.fetch_add(1, std::memory_order_relaxed);
    this->notEmpty.notify_one();
    return true;
  }

  bool enqueue(Task t) {
    {
      std::unique_lock lk(this->m);

      this->notFull.wait(lk, [&] {
        return this->stop || this->count < Cap;
      });

      if (this->stop) {
        return false;
      }

      this->buf[this->tail] = std::move(t);
      this->tail = (this->tail + 1) % Cap;
      this->count += 1;
    }

    this->load.fetch_add(1, std::memory_order_relaxed);
    this->notEmpty.notify_one();
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
    while (true) {
      Task t;

      {
        std::unique_lock lk(this->m);

        this->notEmpty.wait(lk, [&] {
          return this->stop || this->count > 0;
        });

        if (this->stop && this->count == 0) {
          return;
        }

        t = std::move(this->buf[this->head]);
        this->head = (this->head + 1) % Cap;
        this->count -= 1;
      }

      this->notFull.notify_one();

      static_cast<Impl*>(this)->process(t);

      if (t.completion != nullptr) {
        t.completion->signal();
      }

      this->load.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  ~Chan() {
    {
      std::lock_guard lk(this->m);
      this->stop = true;
    }

    this->notEmpty.notify_all();
    this->notFull.notify_all();
    if (this->th.joinable()) {
      this->th.join();
    }
  }
};
