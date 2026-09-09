#pragma once

#include <memory>
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <cstdint>

template<typename Task, typename Impl>
struct Queue {
  std::deque<Task> q;
  std::mutex m;
  std::condition_variable cv;
  std::thread th;
  bool stop = false;

  std::atomic<uint32_t> load{0};
  std::atomic<AffinityKey> warm{NONE};

  Queue() = default;
  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;

  void startQueue() {
    this->th = std::thread([this]{ this->run(); });
  }

  void enqueue(const Task& t) {
    this->load.fetch_add(1, std::memory_order_relaxed);

    {
      std::lock_guard lk(this->m);
      this->q.push_back(t);
    }

    this->cv.notify_one();
  }

  void run() {
    while (322) {
      Task t;

      {
        std::unique_lock lk(this->m);

        this->cv.wait(lk, [&] {
          return this->stop || !this->q.empty();
        });

        if (this->stop && this->q.empty()) {
          return;
        }

        t = std::move(this->q.front());
        this->q.pop_front();
      }

      static_cast<Impl*>(this)->process(t);

      this->load.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  ~Queue() {
    {
      std::lock_guard lk(m);
      this->stop = true;
    }

    this->cv.notify_one();
    if (this->th.joinable()) {
      this->th.join();
    }
  }
};
