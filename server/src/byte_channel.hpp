#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace dicker {

  struct ByteChannel {
    ByteChannel(std::size_t low_water_mark, std::function<void()> resume_notifier);

    void push(const std::uint8_t* data, std::size_t len);
    bool read_exact(std::uint8_t* out, std::size_t count);
    void close();
    std::size_t buffered_bytes();

    std::mutex mutex_;
    std::condition_variable data_available_;
    std::deque<std::vector<std::uint8_t>> blocks_;
    std::size_t front_offset_;
    std::size_t buffered_;
    std::size_t low_water_mark_;
    std::function<void()> resume_notifier_;
    bool closed_;
  };

}
