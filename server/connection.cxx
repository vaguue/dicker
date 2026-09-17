#include "connection.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <string>
#include <vector>
#include <filesystem>
#include <arpa/inet.h>
#include "wire.h"
#include "handshake.h"
#include "file_sink.h"
#include "server.h"
#include "http.h"
#include "archive.h"

namespace dicker {

  namespace {

    const char* status_name(HandshakeStatus status) {
      switch (status) {
        case HandshakeStatus::Ok: return "ok";
        case HandshakeStatus::BadMagic: return "bad-magic";
        case HandshakeStatus::BadVersion: return "bad-version";
        case HandshakeStatus::AuthFailed: return "auth-failed";
        case HandshakeStatus::BadAlgo: return "bad-algo";
        case HandshakeStatus::ServerError: return "server-error";
      }
      return "?";
    }

    std::string hex(const std::uint8_t* d, std::size_t n) {
      static const char* H = "0123456789abcdef";
      std::string s;
      s.reserve(n * 2);
      for (std::size_t i = 0; i < n; ++i) {
        s.push_back(H[d[i] >> 4]);
        s.push_back(H[d[i] & 0x0f]);
      }
      return s;
    }

    const char* algo_name(CompressionAlgo algo) {
      switch (algo) {
        case CompressionAlgo::None: return "none";
        case CompressionAlgo::Lz4: return "lz4";
        case CompressionAlgo::Zstd: return "zstd";
      }
      return "?";
    }

    std::string peer_address(uv_tcp_t* tcp) {
      struct sockaddr_storage addr;
      int len = sizeof(addr);
      char text[128] = {0};
      if (uv_tcp_getpeername(tcp, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
        char ip[INET6_ADDRSTRLEN] = {0};
        int port = 0;
        if (addr.ss_family == AF_INET) {
          auto* v4 = reinterpret_cast<struct sockaddr_in*>(&addr);
          uv_ip4_name(v4, ip, sizeof(ip));
          port = ntohs(v4->sin_port);
        }
        else if (addr.ss_family == AF_INET6) {
          auto* v6 = reinterpret_cast<struct sockaddr_in6*>(&addr);
          uv_ip6_name(v6, ip, sizeof(ip));
          port = ntohs(v6->sin6_port);
        }
        std::snprintf(text, sizeof(text), "%s:%d", ip, port);
      }
      return std::string(text);
    }

    struct ReplyWrite {
      uv_write_t request;
      std::uint8_t data[kHandshakeReplySize];
      Connection* connection;
      bool teardown_after;
    };

    struct TlsWrite {
      uv_write_t request;
      std::vector<std::uint8_t> data;
      Connection* connection;
      bool teardown_after;
    };

    struct DownloadWrite {
      uv_write_t request;
      std::vector<std::uint8_t> data;
      Connection* connection;
    };

    constexpr int kArchiveLevel = 3;                     // zstd level for downloads
    constexpr std::size_t kMaxHttpHeaderBytes = 64u * 1024;  // reject oversized headers

    // A session id names a directory under root; keep it to lowercase hex so it
    // can never escape the root or reference a parent.
    bool valid_session_id(const std::string& id) {
      if (id.empty() || id.size() > 64) {
        return false;
      }
      for (char c : id) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
          return false;
        }
      }
      return true;
    }

    struct UnitSink : DecompressSink {
      bool is_chunk = false;
      SequentialWriter* sequential = nullptr;
      OffsetWriter* offset = nullptr;
      std::uint64_t base_offset = 0;
      std::uint64_t running = 0;
      bool integrity = false;
      XXH64_state_t* checksum_state = nullptr;

      bool write(const std::uint8_t* data, std::size_t len) override {
        if (integrity) {
          XXH64_update(checksum_state, data, len);
        }
        bool ok;
        if (is_chunk) {
          ok = offset->write_at(base_offset + running, data, len);
        }
        else {
          ok = sequential->write(data, len);
        }
        running += len;
        return ok;
      }
    };

  }

  Connection::Connection(Server* server)
    : server_(server),
      state_(State::Handshaking),
      channel_(server->config().channel_low_water, [this]() { uv_async_send(&resume_async_); }),
      integrity_checks_(false),
      units_done_(0),
      bytes_total_(0),
      wire_bytes_(0),
      paused_(false),
      consumer_started_(false),
      teardown_started_(false),
      handles_closing_(false),
      pending_handle_closes_(0) {
  }

  void Connection::accept(Server* server, uv_loop_t* loop, uv_stream_t* listener) {
    Connection* connection = new Connection(server);

    uv_tcp_init(loop, &connection->tcp_);
    connection->tcp_.data = connection;
    uv_async_init(loop, &connection->resume_async_, on_resume_async);
    connection->resume_async_.data = connection;
    uv_async_init(loop, &connection->close_async_, on_close_async);
    connection->close_async_.data = connection;

    if (uv_accept(listener, reinterpret_cast<uv_stream_t*>(&connection->tcp_)) != 0) {
      connection->begin_teardown();
      return;
    }

    connection->peer_ = peer_address(&connection->tcp_);
    std::fprintf(stderr, "conn accepted from %s\n", connection->peer_.c_str());

    uv_read_start(reinterpret_cast<uv_stream_t*>(&connection->tcp_), alloc_cb, on_read);
  }

  void Connection::alloc_cb(uv_handle_t*, std::size_t suggested_size, uv_buf_t* buffer) {
    buffer->base = static_cast<char*>(std::malloc(suggested_size));
    buffer->len = buffer->base != nullptr ? suggested_size : 0;
  }

  void Connection::on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buffer) {
    Connection* connection = static_cast<Connection*>(stream->data);
    if (nread > 0) {
      connection->handle_raw(reinterpret_cast<const std::uint8_t*>(buffer->base),
                             static_cast<std::size_t>(nread));
    }
    else if (nread < 0) {
      connection->begin_teardown();
    }
    if (buffer->base != nullptr) {
      std::free(buffer->base);
    }
  }

  void Connection::handle_raw(const std::uint8_t* data, std::size_t len) {
    if (state_ == State::Handshaking && !tls_detected_) {
      tls_detected_ = true;
      if (len > 0 && data[0] == 0x16) {
        tls_active_ = true;
      }
      std::fprintf(stderr, "conn %s from %s\n", tls_active_ ? "tls" : "plain", peer_.c_str());
      if (tls_active_ && !tls_.init()) {
        std::fprintf(stderr, "tls init failed from %s\n", peer_.c_str());
        begin_teardown();
        return;
      }
    }

    if (tls_active_) {
      tls_.ingest(this, data, len);
    }
    else {
      handle_data(data, len);
    }
  }

  void Connection::handle_data(const std::uint8_t* data, std::size_t len) {
    if (state_ == State::Handshaking) {
      handle_handshake_data(data, len);
    }
    else if (state_ == State::Streaming) {
      wire_bytes_ += len;
      channel_.push(data, len);
      apply_backpressure();
    }
  }

  void Connection::apply_backpressure() {
    if (!paused_ && channel_.buffered_bytes() >= server_->config().channel_high_water) {
      uv_read_stop(reinterpret_cast<uv_stream_t*>(&tcp_));
      paused_ = true;
    }
  }

  void Connection::handle_handshake_data(const std::uint8_t* data, std::size_t len) {
    handshake_buffer_.insert(handshake_buffer_.end(), data, data + len);

    // Decide once whether this is a dicker handshake (begins with kMagic) or an
    // HTTP request. Works identically for plain and TLS-decrypted preambles.
    if (!proto_decided_) {
      std::size_t n = std::min<std::size_t>(handshake_buffer_.size(), kMagic.size());
      if (std::memcmp(handshake_buffer_.data(), kMagic.data(), n) != 0) {
        proto_decided_ = true;
        http_mode_ = true;
      }
      else if (handshake_buffer_.size() >= kMagic.size()) {
        proto_decided_ = true;
        http_mode_ = false;
      }
      else {
        return;  // still ambiguous; wait for more bytes
      }
    }

    if (http_mode_) {
      handle_http_data();
      return;
    }

    if (handshake_buffer_.size() < kHandshakeRequestSize) {
      return;
    }

    std::fprintf(stderr, "recv handshake(%zu)=%s from %s (keylen=%zu)\n",
                 static_cast<std::size_t>(kHandshakeRequestSize),
                 hex(handshake_buffer_.data(), kHandshakeRequestSize).c_str(),
                 peer_.c_str(), server_->config().key.size());

    HandshakeRequest request;
    HandshakeStatus status = parse_and_verify_handshake(handshake_buffer_.data(),
                                                        server_->config().key, request);

    if (status == HandshakeStatus::Ok) {
      decompressor_ = make_decompressor(request.algo);
      sink_ = server_->get_or_create_sink(request.session_id);
      if (!decompressor_ || !sink_) {
        status = HandshakeStatus::ServerError;
      }
    }

    send_handshake_reply(status);
    if (status != HandshakeStatus::Ok) {
      std::fprintf(stderr, "handshake rejected from %s: %s\n",
                   peer_.c_str(), status_name(status));
      return;
    }

    integrity_checks_ = has_flag(request.flags, HandshakeFlag::IntegrityChecks);
    session_hex_ = session_id_to_hex(request.session_id);
    std::fprintf(stderr, "handshake ok from %s session=%s algo=%s integrity=%d\n",
                 peer_.c_str(), session_hex_.c_str(), algo_name(request.algo),
                 integrity_checks_ ? 1 : 0);
    state_ = State::Streaming;

    // Record uploader IP + first-seen time for GET /uploads (best effort).
    {
      std::string ip = peer_;
      std::size_t colon = ip.find_last_of(':');
      if (colon != std::string::npos) {
        ip.resize(colon);
      }
      server_->record_session(session_hex_, ip);
    }

    std::vector<std::uint8_t> leftover(handshake_buffer_.begin() + kHandshakeRequestSize,
                                       handshake_buffer_.end());
    handshake_buffer_.clear();
    handshake_buffer_.shrink_to_fit();

    consumer_started_ = true;
    consumer_ = std::thread([this]() { consumer_main(); });

    if (!leftover.empty()) {
      channel_.push(leftover.data(), leftover.size());
      apply_backpressure();
    }
  }

  void Connection::handle_http_data() {
    if (http_done_) {
      return;  // response already dispatched; ignore trailing bytes
    }
    std::size_t header_end =
        http_header_end(handshake_buffer_.data(), handshake_buffer_.size());
    if (header_end == 0) {
      if (handshake_buffer_.size() > kMaxHttpHeaderBytes) {
        http_done_ = true;
        send_http_simple(400, "Bad Request", "text/plain; charset=utf-8",
                         "bad request\n");
      }
      return;  // wait for the full header block
    }

    HttpRequest request;
    if (!parse_http_request(handshake_buffer_.data(), header_end, request)) {
      http_done_ = true;
      send_http_simple(400, "Bad Request", "text/plain; charset=utf-8",
                       "bad request\n");
      return;
    }

    http_done_ = true;
    std::fprintf(stderr, "http %s %s from %s\n", request.method.c_str(),
                 request.path.c_str(), peer_.c_str());
    route_http(request);
  }

  bool Connection::authorized(const HttpRequest& request) const {
    const std::string* auth = request.header("authorization");
    if (auth == nullptr) {
      return false;
    }
    static const std::string prefix = "Bearer ";
    if (auth->size() <= prefix.size() ||
        auth->compare(0, prefix.size(), prefix) != 0) {
      return false;
    }
    return server_->verify_token(auth->substr(prefix.size()));
  }

  void Connection::route_http(const HttpRequest& request) {
    if (request.method != "GET") {
      send_http_simple(405, "Method Not Allowed", "text/plain; charset=utf-8",
                       "method not allowed\n");
      return;
    }

    if (request.path == "/") {
      serve_root();
      return;
    }

    if (request.path == "/uploads") {
      if (!authorized(request)) {
        send_http_simple(401, "Unauthorized", "text/plain; charset=utf-8",
                         "unauthorized\n", {{"WWW-Authenticate", "Bearer"}});
        return;
      }
      serve_uploads_list();
      return;
    }

    static const std::string prefix = "/uploads/";
    if (request.path.compare(0, prefix.size(), prefix) == 0) {
      if (!authorized(request)) {
        send_http_simple(401, "Unauthorized", "text/plain; charset=utf-8",
                         "unauthorized\n", {{"WWW-Authenticate", "Bearer"}});
        return;
      }
      serve_session_download(request.path.substr(prefix.size()));
      return;
    }

    send_http_simple(404, "Not Found", "text/plain; charset=utf-8", "not found\n");
  }

  void Connection::serve_root() {
    static const std::string body =
        "<!doctype html>\n<html><head><meta charset=\"utf-8\">"
        "<title>dicker</title></head>"
        "<body><h1>dicker</h1><p>hello world</p></body></html>\n";
    send_http_simple(200, "OK", "text/html; charset=utf-8", body);
  }

  void Connection::serve_uploads_list() {
    // Straight from the append-only session log — no disk walk. A session whose
    // directory has since disappeared simply errors at download time.
    std::map<std::string, SessionInfo> sessions;
    server_->load_sessions(sessions);

    std::string body = "[";
    bool first = true;
    for (const auto& entry : sessions) {
      if (!first) {
        body.push_back(',');
      }
      first = false;
      body += "{\"sessionId\":\"";
      body += entry.first;
      body += "\",\"ip\":\"";
      body += entry.second.ip;
      body += "\",\"created\":";
      body += std::to_string(entry.second.created);
      body += "}";
    }
    body += "]";

    send_http_simple(200, "OK", "application/json", body);
  }

  void Connection::serve_session_download(const std::string& session_id) {
    if (!valid_session_id(session_id)) {
      send_http_simple(404, "Not Found", "text/plain; charset=utf-8",
                       "not found\n");
      return;
    }
    std::filesystem::path dir = server_->config().root / session_id;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      send_http_simple(404, "Not Found", "text/plain; charset=utf-8",
                       "not found\n");
      return;
    }
    begin_download_response(session_id, dir.string());
  }

  void Connection::send_http_simple(
      int status, const char* reason, const std::string& content_type,
      const std::string& body,
      const std::vector<std::pair<std::string, std::string>>& extra) {
    std::vector<std::pair<std::string, std::string>> headers = extra;
    headers.emplace_back("Connection", "close");

    std::string head = http_response_headers(
        status, reason, content_type, static_cast<long long>(body.size()), headers);

    std::vector<std::uint8_t> resp(head.begin(), head.end());
    resp.insert(resp.end(), body.begin(), body.end());

    // Send the whole response in a single write so teardown happens only after
    // all of it is out (flush_out would tear down after its first 16 KB chunk).
    if (tls_active_) {
      std::vector<std::uint8_t> wire;
      if (!tls_.encrypt(resp.data(), resp.size(), wire)) {
        begin_teardown();
        return;
      }
      tls_write(wire.data(), wire.size(), true);
    }
    else {
      tls_write(resp.data(), resp.size(), true);
    }
  }

  void Connection::begin_download_response(const std::string& session_id,
                                           const std::string& dir_path) {
    download_ = std::make_unique<ArchiveStreamer>();
    if (!download_->begin(dir_path, session_id, kArchiveLevel)) {
      download_.reset();
      send_http_simple(500, "Internal Server Error", "text/plain; charset=utf-8",
                       "archive error\n");
      return;
    }

    std::string disposition =
        "attachment; filename=\"" + session_id + ".tar.zst\"";
    std::string head = http_response_headers(
        200, "OK", "application/zstd", -1,
        {{"Content-Disposition", disposition}, {"Connection", "close"}});

    download_last_ = false;
    download_send(std::vector<std::uint8_t>(head.begin(), head.end()));
  }

  void Connection::pump_download() {
    if (!download_) {
      begin_teardown();
      return;
    }
    std::vector<std::uint8_t> chunk;
    bool more = download_->next(chunk);
    if (!download_->ok()) {
      std::fprintf(stderr, "download archive error from %s\n", peer_.c_str());
      begin_teardown();
      return;
    }
    if (chunk.empty()) {
      // Archive fully emitted with no trailing bytes.
      begin_teardown();
      return;
    }
    download_last_ = !more;
    download_send(std::move(chunk));
  }

  void Connection::download_send(std::vector<std::uint8_t>&& plain) {
    if (tls_active_) {
      std::vector<std::uint8_t> wire;
      if (!tls_.encrypt(plain.data(), plain.size(), wire)) {
        begin_teardown();
        return;
      }
      download_write(std::move(wire));
    }
    else {
      download_write(std::move(plain));
    }
  }

  void Connection::download_write(std::vector<std::uint8_t>&& bytes) {
    if (bytes.empty()) {
      // Nothing to send this step; advance as if the write had completed.
      if (download_last_) {
        begin_teardown();
      }
      else {
        pump_download();
      }
      return;
    }

    DownloadWrite* write = new DownloadWrite();
    write->data = std::move(bytes);
    write->connection = this;
    write->request.data = write;

    uv_buf_t buffer = uv_buf_init(reinterpret_cast<char*>(write->data.data()),
                                  static_cast<unsigned int>(write->data.size()));
    int result = uv_write(&write->request, reinterpret_cast<uv_stream_t*>(&tcp_),
                          &buffer, 1, on_write_download);
    if (result != 0) {
      delete write;
      begin_teardown();
    }
  }

  void Connection::on_write_download(uv_write_t* request, int status) {
    DownloadWrite* write = static_cast<DownloadWrite*>(request->data);
    Connection* connection = write->connection;
    bool last = connection->download_last_;
    delete write;

    if (status != 0 || last) {
      connection->begin_teardown();
      return;
    }
    connection->pump_download();
  }

  void Connection::send_handshake_reply(HandshakeStatus status) {
    if (tls_active_) {
      std::uint8_t data[kHandshakeReplySize];
      std::memcpy(data, kMagic.data(), kMagic.size());
      data[kMagic.size()] = static_cast<std::uint8_t>(status);

      if (!tls_.send(this, data, sizeof data, status != HandshakeStatus::Ok)) {
        begin_teardown();
        return;
      }
      return;
    }

    ReplyWrite* reply = new ReplyWrite();
    std::memcpy(reply->data, kMagic.data(), kMagic.size());
    reply->data[kMagic.size()] = static_cast<std::uint8_t>(status);
    reply->connection = this;
    reply->teardown_after = status != HandshakeStatus::Ok;
    reply->request.data = reply;

    uv_buf_t buffer = uv_buf_init(reinterpret_cast<char*>(reply->data), kHandshakeReplySize);
    int result = uv_write(&reply->request, reinterpret_cast<uv_stream_t*>(&tcp_),
                          &buffer, 1, on_write_reply);
    if (result != 0) {
      delete reply;
      begin_teardown();
    }
  }

  void Connection::tls_write(const std::uint8_t* data, std::size_t len, bool teardown_after) {
    TlsWrite* write = new TlsWrite();
    write->data.assign(data, data + len);
    write->connection = this;
    write->teardown_after = teardown_after;
    write->request.data = write;

    uv_buf_t buffer = uv_buf_init(reinterpret_cast<char*>(write->data.data()),
                                  static_cast<unsigned int>(write->data.size()));
    int result = uv_write(&write->request, reinterpret_cast<uv_stream_t*>(&tcp_),
                          &buffer, 1, on_write_tls);
    if (result != 0) {
      delete write;
      begin_teardown();
    }
  }

  void Connection::on_write_tls(uv_write_t* request, int status) {
    TlsWrite* write = static_cast<TlsWrite*>(request->data);
    Connection* connection = write->connection;
    bool teardown_after = write->teardown_after || status != 0;
    delete write;
    if (teardown_after) {
      connection->begin_teardown();
    }
  }

  void Connection::on_write_reply(uv_write_t* request, int status) {
    ReplyWrite* reply = static_cast<ReplyWrite*>(request->data);
    Connection* connection = reply->connection;
    bool teardown_after = reply->teardown_after || status != 0;
    delete reply;
    if (teardown_after) {
      connection->begin_teardown();
    }
  }

  void Connection::on_resume_async(uv_async_t* handle) {
    Connection* connection = static_cast<Connection*>(handle->data);
    if (connection->state_ == State::Streaming && connection->paused_) {
      connection->paused_ = false;
      uv_read_start(reinterpret_cast<uv_stream_t*>(&connection->tcp_), alloc_cb, on_read);
    }
  }

  void Connection::consumer_main() {
    // The streaming phase now opens with a Cmd byte selecting what the client
    // wants to do; each command is handled by its own function.
    std::uint8_t cmd_byte;
    if (channel_.read_exact(&cmd_byte, 1)) {
      switch (static_cast<Cmd>(cmd_byte)) {
        case Cmd::Upload:
          handle_upload();
          break;
        default:
          std::fprintf(stderr, "unknown cmd 0x%02x session=%s from %s\n",
                       cmd_byte, session_hex_.c_str(), peer_.c_str());
          break;
      }
    }

    uv_async_send(&close_async_);
  }

  void Connection::handle_upload() {
    std::vector<std::uint8_t> input_buffer;

    while (true) {
      std::uint8_t type_byte;
      if (!channel_.read_exact(&type_byte, 1)) {
        break;
      }
      UnitType type = static_cast<UnitType>(type_byte);
      if (type == UnitType::End) {
        break;
      }
      if (type != UnitType::File && type != UnitType::Chunk) {
        break;
      }

      std::uint8_t path_len_bytes[2];
      if (!channel_.read_exact(path_len_bytes, 2)) {
        break;
      }
      std::uint16_t path_len = read_u16_le(path_len_bytes);
      if (path_len == 0 || path_len > kMaxPathLength) {
        break;
      }

      std::string path(path_len, '\0');
      if (!channel_.read_exact(reinterpret_cast<std::uint8_t*>(path.data()), path_len)) {
        break;
      }

      std::uint64_t base_offset = 0;
      if (type == UnitType::Chunk) {
        std::uint8_t offset_bytes[8];
        if (!channel_.read_exact(offset_bytes, 8)) {
          break;
        }
        base_offset = read_u64_le(offset_bytes);
      }

      if (type == UnitType::Chunk) {
        std::fprintf(stderr, "recv chunk session=%s path=%s offset=%llu\n",
                     session_hex_.c_str(), path.c_str(),
                     static_cast<unsigned long long>(base_offset));
      }
      else {
        std::fprintf(stderr, "recv file session=%s path=%s\n",
                     session_hex_.c_str(), path.c_str());
      }

      SequentialWriter sequential;
      OffsetWriter offset;
      if (type == UnitType::File) {
        sequential = sink_->open_sequential(path);
        if (!sequential.valid()) {
          std::fprintf(stderr, "reject session=%s path=%s (open failed / not permitted)\n",
                       session_hex_.c_str(), path.c_str());
          break;
        }
      }
      else {
        offset = sink_->open_offset(path);
        if (!offset.valid()) {
          std::fprintf(stderr, "reject session=%s path=%s (open failed / not permitted)\n",
                       session_hex_.c_str(), path.c_str());
          break;
        }
      }

      XXH64_state_t checksum_state;
      if (integrity_checks_) {
        XXH64_reset(&checksum_state, 0);
      }

      UnitSink unit_sink;
      unit_sink.is_chunk = type == UnitType::Chunk;
      unit_sink.sequential = &sequential;
      unit_sink.offset = &offset;
      unit_sink.base_offset = base_offset;
      unit_sink.running = 0;
      unit_sink.integrity = integrity_checks_;
      unit_sink.checksum_state = &checksum_state;

      bool unit_ok = true;
      bool unit_done = false;
      while (true) {
        std::uint8_t block_type;
        if (!channel_.read_exact(&block_type, 1)) {
          unit_ok = false;
          break;
        }
        if (static_cast<BlockType>(block_type) == BlockType::EndOfUnit) {
          unit_done = true;
          break;
        }
        if (static_cast<BlockType>(block_type) != BlockType::Data) {
          unit_ok = false;
          break;
        }

        std::uint8_t block_len_bytes[4];
        if (!channel_.read_exact(block_len_bytes, 4)) {
          unit_ok = false;
          break;
        }
        std::uint32_t block_len = read_u32_le(block_len_bytes);
        if (block_len == 0 || block_len > kMaxBlockSize) {
          unit_ok = false;
          break;
        }

        input_buffer.resize(block_len);
        if (!channel_.read_exact(input_buffer.data(), block_len)) {
          unit_ok = false;
          break;
        }

        if (!decompressor_->decompress(input_buffer.data(), block_len, unit_sink)) {
          std::fprintf(stderr, "decode error session=%s path=%s\n",
                       session_hex_.c_str(), path.c_str());
          unit_ok = false;
          break;
        }
      }

      if (unit_ok && unit_done && integrity_checks_) {
        std::uint8_t checksum_bytes[8];
        if (!channel_.read_exact(checksum_bytes, 8)) {
          unit_ok = false;
        }
        else if (read_u64_le(checksum_bytes) != XXH64_digest(&checksum_state)) {
          std::fprintf(stderr, "checksum mismatch session=%s path=%s\n",
                       session_hex_.c_str(), path.c_str());
          unit_ok = false;
        }
      }

      if (type == UnitType::File) {
        sequential.close();
      }

      if (!unit_ok || !unit_done) {
        break;
      }

      units_done_ += 1;
      bytes_total_ += unit_sink.running;
      std::fprintf(stderr, "ok session=%s path=%s bytes=%llu\n",
                   session_hex_.c_str(), path.c_str(),
                   static_cast<unsigned long long>(unit_sink.running));
    }
  }

  void Connection::begin_teardown() {
    if (teardown_started_) {
      return;
    }
    teardown_started_ = true;
    state_ = State::Closing;

    if (consumer_started_) {
      channel_.close();
    }
    else {
      finalize_close();
    }
  }

  void Connection::on_close_async(uv_async_t* handle) {
    Connection* connection = static_cast<Connection*>(handle->data);
    connection->teardown_started_ = true;
    connection->state_ = State::Closing;
    connection->finalize_close();
  }

  void Connection::finalize_close() {
    if (handles_closing_) {
      return;
    }
    handles_closing_ = true;

    close_handle(reinterpret_cast<uv_handle_t*>(&tcp_));
    close_handle(reinterpret_cast<uv_handle_t*>(&resume_async_));
    close_handle(reinterpret_cast<uv_handle_t*>(&close_async_));
  }

  void Connection::close_handle(uv_handle_t* handle) {
    if (!uv_is_closing(handle)) {
      pending_handle_closes_ += 1;
      uv_close(handle, on_handle_closed);
    }
  }

  void Connection::on_handle_closed(uv_handle_t* handle) {
    Connection* connection = static_cast<Connection*>(handle->data);
    connection->pending_handle_closes_ -= 1;
    if (connection->pending_handle_closes_ == 0) {
      if (connection->consumer_started_ && connection->consumer_.joinable()) {
        connection->consumer_.join();
      }
      double ratio = connection->wire_bytes_ > 0
        ? static_cast<double>(connection->bytes_total_) / static_cast<double>(connection->wire_bytes_)
        : 0.0;
      std::fprintf(stderr,
                   "conn closed %s session=%s units=%llu bytes=%llu wire=%llu ratio=%.2fx\n",
                   connection->peer_.c_str(),
                   connection->session_hex_.empty() ? "-" : connection->session_hex_.c_str(),
                   static_cast<unsigned long long>(connection->units_done_),
                   static_cast<unsigned long long>(connection->bytes_total_),
                   static_cast<unsigned long long>(connection->wire_bytes_),
                   ratio);
      delete connection;
    }
  }

}
