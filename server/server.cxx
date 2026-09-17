#include "server.h"

#include <cstdio>
#include <ctime>

#include "connection.h"
#include "file_sink.h"
#include "handshake.h"

namespace dicker {

  Server::Server(uv_loop_t* loop, ServerConfig config)
    : loop_(loop),
      config_(std::move(config)) {
    tokens_path_ = (config_.root / ".dicker-tokens.json").string();
    sessions_path_ = (config_.root / ".dicker-sessions.jsonl").string();

    // Probe the token store once, only to surface key problems in the log; it is
    // reloaded on every check, so nothing is cached.
    TokenStore probe;
    std::string err;
    if (TokenStore::load(tokens_path_, config_.key, probe, err)) {
      std::fprintf(stderr, "[db] %zu token(s) at %s\n", probe.tokens.size(),
                   tokens_path_.c_str());
    }
    else {
      std::fprintf(stderr,
                   "[db] %s: %s — HTTP auth disabled until fixed\n",
                   tokens_path_.c_str(), err.c_str());
    }

    // Preload the ids already in the session log so we never append duplicates.
    std::map<std::string, SessionInfo> known;
    SessionLog::load_all(sessions_path_, config_.key, known);
    for (const auto& entry : known) {
      logged_sessions_.insert(entry.first);
    }
    std::fprintf(stderr, "[db] %zu session(s) at %s\n", logged_sessions_.size(),
                 sessions_path_.c_str());
  }

  bool Server::verify_token(const std::string& token) const {
    TokenStore tokens;
    std::string err;
    if (!TokenStore::load(tokens_path_, config_.key, tokens, err)) {
      return false;  // missing/corrupt token file -> deny
    }
    return tokens.verify(token);
  }

  void Server::record_session(const std::string& hex, const std::string& ip) {
    if (logged_sessions_.count(hex) != 0) {
      return;  // already logged this run / preloaded from the log
    }
    std::string err;
    if (SessionLog::append(sessions_path_, config_.key, hex, ip,
                           static_cast<std::uint64_t>(std::time(nullptr)), err)) {
      logged_sessions_.insert(hex);
    }
    else {
      std::fprintf(stderr, "[db] session append failed: %s\n", err.c_str());
    }
  }

  bool Server::load_sessions(std::map<std::string, SessionInfo>& out) const {
    return SessionLog::load_all(sessions_path_, config_.key, out);
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
