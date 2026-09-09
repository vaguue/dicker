#include "byte_channel.hpp"

#include <cstring>

namespace dicker {

  ByteChannel::ByteChannel(std::size_t low_water_mark, std::function<void()> resume_notifier)
    : front_offset_(0),
      buffered_(0),
      low_water_mark_(low_water_mark),
      resume_notifier_(std::move(resume_notifier)),
      closed_(false) {
  }

  void ByteChannel::push(const std::uint8_t* data, std::size_t len) {
    if (len == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    blocks_.emplace_back(data, data + len);
    buffered_ += len;
    data_available_.notify_one();
  }

  bool ByteChannel::read_exact(std::uint8_t* out, std::size_t count) {
    std::size_t copied = 0;
    bool signaled = false;
    std::unique_lock<std::mutex> lock(mutex_);

    while (copied < count) {
      while (blocks_.empty() && !closed_) {
        data_available_.wait(lock);
      }
      if (blocks_.empty() && closed_) {
        return false;
      }

      std::vector<std::uint8_t>& front = blocks_.front();
      std::size_t available = front.size() - front_offset_;
      std::size_t needed = count - copied;
      std::size_t take = needed < available ? needed : available;

      std::memcpy(out + copied, front.data() + front_offset_, take);
      copied += take;
      front_offset_ += take;
      buffered_ -= take;

      if (front_offset_ == front.size()) {
        blocks_.pop_front();
        front_offset_ = 0;
      }

      if (!signaled && buffered_ <= low_water_mark_ && resume_notifier_) {
        resume_notifier_();
        signaled = true;
      }
    }

    return true;
  }

  void ByteChannel::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    data_available_.notify_all();
  }

  std::size_t ByteChannel::buffered_bytes() {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffered_;
  }

}
