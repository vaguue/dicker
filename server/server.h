#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <array>
#include <memory>
#include <mutex>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <filesystem>
#include <uv.h>
#include "protocol.h"
#include "db.h"
#include "router.h"

namespace dicker {

  struct FileSink;

  namespace server {

    enum struct ProbeHash {
      none,
      blake3,
    };

    // What a client wants to upload, asked before any data is sent.
    struct Probe {
      std::string_view session;
      std::string_view peer;
      std::string_view path;
      UnitType type;               // File, or Chunk of a big file
      std::uint64_t offset;        // where the chunk starts; 0 for a file
      std::uint64_t length;        // bytes in this unit
      std::uint64_t fileSize;      // size of the whole file
      std::int64_t mtime;          // last write time, nanoseconds since the Unix epoch
      std::span<const std::uint8_t> hash;  // BLAKE3 of the unit; empty unless ProbeHash::blake3
    };

    // Returns true to accept the unit. Runs on the connection's thread, possibly
    // for several connections at once, so it has to be thread-safe.
    using Filter = std::function<bool(const Probe&)>;

  }

  struct ServerConfig {
    std::string key;
    std::filesystem::path root;
    std::size_t channel_high_water = 8u * 1024 * 1024;  // stop reading a connection's socket at this many buffered bytes
    std::size_t channel_low_water = 2u * 1024 * 1024;   // resume once the buffer drains to this
    bool default_routes = true;  // serve /, /uploads and /uploads/...
  };

  struct Server {
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool listen(const std::string& address, int port);

    // Serves until stop() is called.
    void run();

    // Makes run() return. Safe to call from any thread, including a route
    // handler. Meant for shutdown: uploads still in flight are cut off.
    void stop();

    // Adds an HTTP endpoint, checked before the built-in routes. Plain HTTP and
    // HTTPS are both served on the listening port. Register before listen().
    void route(std::string method, std::string path, HttpHandler handler, Chain middleware = {}) {
      router.add(std::move(method), Match::Exact, std::move(path), std::move(handler), std::move(middleware));
    }

    void route(std::string method, Match match, std::string path, HttpHandler handler, Chain middleware = {}) {
      router.add(std::move(method), match, std::move(path), std::move(handler), std::move(middleware));
    }

    // Puts the server in accept mode: protocol-2 clients probe every unit and
    // upload only what `filter` accepts. Protocol-1 clients are not filtered.
    // Set before listen().
    void filter(server::Filter callback, server::ProbeHash hash = server::ProbeHash::none) {
      filter_ = std::move(callback);
      probe_hash_ = hash;
    }

    std::uint8_t mode_for(std::uint16_t version) const {
      if (version < 2 || !filter_) {
        return 0;
      }

      std::uint8_t mode = static_cast<std::uint8_t>(ServerMode::Accept);

      if (probe_hash_ == server::ProbeHash::blake3) {
        mode |= static_cast<std::uint8_t>(ServerMode::HashFirst);
      }
      return mode;
    }

    const ServerConfig& config() const {
      return config_;
    }

    std::shared_ptr<FileSink> get_or_create_sink(const std::array<std::uint8_t, kSessionIdSize>& session_id);

    // Token/session persistence (loop thread only). Tokens live in an encrypted
    // whole-file store reloaded on every check, so dicker-dbtool changes take
    // effect live without a restart. Sessions live in an append-only encrypted
    // log; each one is recorded once (deduped via logged_sessions_).
    bool verify_token(const std::string& token) const;
    void record_session(const std::string& hex, const std::string& ip);
    bool load_sessions(std::map<std::string, SessionInfo>& out) const;

    static void on_new_connection(uv_stream_t* server_handle, int status);
    static void on_stop(uv_async_t* handle);

    uv_loop_t loop_;
    uv_async_t stop_async_;
    bool tcp_initialized_ = false;
    ServerConfig config_;
    Router router;
    server::Filter filter_;
    server::ProbeHash probe_hash_ = server::ProbeHash::none;
    uv_tcp_t tcp_server_;
    std::mutex sinks_mutex_;
    std::unordered_map<std::string, std::shared_ptr<FileSink>> sinks_;

    std::string tokens_path_;
    std::string sessions_path_;
    std::unordered_set<std::string> logged_sessions_;
  };

}
