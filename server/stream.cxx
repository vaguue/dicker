#include "stream.h"

#include <utility>

namespace dicker {

  bool Stream::write(std::string_view bytes) {
    std::unique_lock<std::mutex> lock(mutex);
    drained.wait(lock, [&]() { return closed || queued.size() < highWater; });
    if (closed || ended || failed) {
      return false;
    }
    queued.append(bytes);
    if (wake) {
      wake();
    }
    return true;
  }

  void Stream::end() {
    std::lock_guard<std::mutex> lock(mutex);
    ended = true;
    if (wake) {
      wake();
    }
  }

  void Stream::fail(int status, std::string type, std::string body) {
    std::lock_guard<std::mutex> lock(mutex);
    failed = true;
    failStatus = status;
    failType = std::move(type);
    failBody = std::move(body);
    if (wake) {
      wake();
    }
  }

  void Stream::attach(std::function<void()> connectionWake) {
    std::lock_guard<std::mutex> lock(mutex);
    wake = std::move(connectionWake);
  }

  void Stream::detach() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      closed = true;
      wake = nullptr;
    }
    drained.notify_all();
  }

}
