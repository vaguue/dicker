#include "server.hpp"

#include "connection.hpp"
#include "file_sink.hpp"
#include "handshake.hpp"

namespace dicker {

  Server::Server(uv_loop_t* loop, ServerConfig config)
    : loop_(loop),
      config_(std::move(config)) {
  }

  bool Server::listen(const std::string& address, int port) {
    if (uv_tcp_init(loop_, &tcp_server_) != 0) {
      return false;
    }
    tcp_server_.data = this;

    struct sockaddr_in bind_address;
    if (uv_ip4_addr(address.c_str(), port, &bind_address) != 0) {
      return false;
    }
    if (uv_tcp_bind(&tcp_server_, reinterpret_cast<const struct sockaddr*>(&bind_address), 0) != 0) {
      return false;
    }
    if (uv_listen(reinterpret_cast<uv_stream_t*>(&tcp_server_), 128, on_new_connection) != 0) {
      return false;
    }
    return true;
  }

  void Server::on_new_connection(uv_stream_t* server_handle, int status) {
    if (status != 0) {
      return;
    }
    Server* self = static_cast<Server*>(server_handle->data);
    Connection::accept(self, self->loop_, server_handle);
  }

  std::shared_ptr<FileSink> Server::get_or_create_sink(const std::array<std::uint8_t, kSessionIdSize>& session_id) {
    std::string hex = session_id_to_hex(session_id);
    std::lock_guard<std::mutex> lock(sinks_mutex_);

    auto existing = sinks_.find(hex);
    if (existing != sinks_.end()) {
      return existing->second;
    }

    std::filesystem::path session_root = config_.root / hex;
    std::error_code error;
    std::filesystem::create_directories(session_root, error);
    if (error && !std::filesystem::exists(session_root)) {
      return nullptr;
    }

    auto sink = std::make_shared<FileSink>(loop_, session_root);
    sinks_.emplace(hex, sink);
    return sink;
  }

}
