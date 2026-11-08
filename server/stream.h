#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace dicker {

  /// A response body written from any thread, Node-style: a handler sets `ctx.body = stream`, returns, and a worker
  /// calls write() and then end() or fail(). The status, type and headers the handler left on `ctx` go out with the
  /// first bytes, as a chunked response, so a fail() before any write() still answers with its own status. write()
  /// blocks while `highWater` bytes wait for the socket and returns false once the client is gone. A stream that is
  /// never ended keeps its connection open.
  struct Stream {
    std::size_t highWater = 4u * 1024 * 1024;

    std::mutex mutex;
    std::condition_variable drained;
    std::string queued;
    bool ended = false;
    bool failed = false;
    bool closed = false;
    int failStatus = 500;
    std::string failType;
    std::string failBody;
    std::function<void()> wake;

    bool write(std::string_view bytes);
    void end();
    void fail(int status, std::string type, std::string body);

    void attach(std::function<void()> connectionWake);
    void detach();
  };

}
