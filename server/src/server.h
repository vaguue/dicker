#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <uv.h>
#include "protocol.h"

namespace dicker {

  struct FileSink;

  struct ServerConfig {
    std::string key;
    std::filesystem::path root;
    std::size_t channel_high_water;
    std::size_t channel_low_water;
  };

  struct Server {
    Server(uv_loop_t* loop, ServerConfig config);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool listen(const std::string& address, int port);

    const ServerConfig& config() const {
      return config_;
    }

    std::shared_ptr<FileSink> get_or_create_sink(const std::array<std::uint8_t, kSessionIdSize>& session_id);

    static void on_new_connection(uv_stream_t* server_handle, int status);

    uv_loop_t* loop_;
    ServerConfig config_;
    uv_tcp_t tcp_server_;
    std::mutex sinks_mutex_;
    std::unordered_map<std::string, std::shared_ptr<FileSink>> sinks_;
  };

}
