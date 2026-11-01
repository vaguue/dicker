#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <array>
#include <memory>
#include <mutex>
#include <map>
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

  struct ServerConfig {
    std::string key;
    std::filesystem::path root;
    std::size_t channel_high_water;
    std::size_t channel_low_water;
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
    uv_tcp_t tcp_server_;
    std::mutex sinks_mutex_;
    std::unordered_map<std::string, std::shared_ptr<FileSink>> sinks_;

    std::string tokens_path_;
    std::string sessions_path_;
    std::unordered_set<std::string> logged_sessions_;
  };

}
