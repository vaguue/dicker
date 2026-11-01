#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace timers {
namespace detail {

struct Timer {
  std::mutex m;
  std::condition_variable cv;
  bool cancelled = false;
};

struct Registry {
  static Registry& get() {
    static Registry r;
    return r;
  }

  std::mutex mu_;
  std::condition_variable done_;
  std::unordered_map<int, std::shared_ptr<Timer>> live_;
  int last_id_ = 0;
  int active_ = 0;

  Registry() = default;

  void finish(int id) {
    std::lock_guard<std::mutex> g(mu_);
    live_.erase(id);
    if (--active_ == 0) done_.notify_all();
  }

  int start(std::chrono::milliseconds period, std::function<void()> fn, bool repeat) {
    auto t = std::make_shared<Timer>();
    int id;

    {
      std::lock_guard<std::mutex> g(mu_);
      id = ++last_id_;
      live_[id] = t;
      ++active_;
    }

    std::thread([this, t, period, fn, repeat, id] {
      auto next = std::chrono::steady_clock::now();
      for (;;) {
        next += period;
        auto now = std::chrono::steady_clock::now();
        if (next < now) next = now;

        {
          std::unique_lock<std::mutex> lk(t->m);
          if (t->cv.wait_until(lk, next, [&] { return t->cancelled; }))
            break;
        }

        // An exception in the callback must not terminate the process: this
        // thread is detached, so std::terminate would take the whole program
        // down with it. Report and keep the timer running.
        try {
          fn();
        } catch (const std::exception& failure) {
          std::fprintf(stderr, "[!] timer callback threw: %s\n", failure.what());
        } catch (...) {
          std::fprintf(stderr, "[!] timer callback threw\n");
        }
        if (!repeat) break;
      }
      finish(id);
    }).detach();

    return id;
  }

  void stop(int id) {
    std::shared_ptr<Timer> t;
    {
      std::lock_guard<std::mutex> g(mu_);
      auto it = live_.find(id);
      if (it == live_.end()) return;
      t = it->second;
    }
    {
      std::lock_guard<std::mutex> g(t->m);
      t->cancelled = true;
    }
    t->cv.notify_all();
  }

  void stopAll() {
    std::vector<std::shared_ptr<Timer>> all;
    {
      std::lock_guard<std::mutex> g(mu_);
      for (auto& kv : live_) all.push_back(kv.second);
    }
    for (auto& t : all) {
      {
        std::lock_guard<std::mutex> g(t->m);
        t->cancelled = true;
      }
      t->cv.notify_all();
    }
  }

  void wait() {
    std::unique_lock<std::mutex> lk(mu_);
    done_.wait(lk, [&] { return active_ == 0; });
  }
};

}

inline int setInterval(std::function<void()> fn, std::chrono::milliseconds period) {
  return detail::Registry::get().start(period, std::move(fn), true);
}

inline int setTimeout(std::function<void()> fn, std::chrono::milliseconds delay) {
  return detail::Registry::get().start(delay, std::move(fn), false);
}

inline void clearInterval(int id) { detail::Registry::get().stop(id); }
inline void clearTimeout(int id) { detail::Registry::get().stop(id); }

inline void clearAll() { detail::Registry::get().stopAll(); }

inline void wait() { detail::Registry::get().wait(); }

thread_local std::chrono::steady_clock::time_point g_start;

void mStart() {
  g_start = std::chrono::steady_clock::now();
}

void mEnd(const char* Tag) {
  auto res = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
  dicker::log().info("%s took %lldms", Tag, res);
}

struct Throttle {
  std::chrono::steady_clock::duration interval;
  const char* tag = "";
  std::chrono::steady_clock::time_point last{};

  bool ready() {
    auto now = std::chrono::steady_clock::now();

    if (now - last < interval) {
      dicker::log().info("%s throttled @_@", this->tag);
      return false;
    }

    last = now;

    return true;
  }
};

}
