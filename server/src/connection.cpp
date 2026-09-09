#include "connection.hpp"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "wire.hpp"
#include "handshake.hpp"
#include "file_sink.hpp"
#include "server.hpp"

namespace dicker {

  namespace {

    struct ReplyWrite {
      uv_write_t request;
      std::uint8_t data[kHandshakeReplySize];
      Connection* connection;
      bool teardown_after;
    };

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

    uv_read_start(reinterpret_cast<uv_stream_t*>(&connection->tcp_), alloc_cb, on_read);
  }

  void Connection::alloc_cb(uv_handle_t*, std::size_t suggested_size, uv_buf_t* buffer) {
    buffer->base = static_cast<char*>(std::malloc(suggested_size));
    buffer->len = buffer->base != nullptr ? suggested_size : 0;
  }

  void Connection::on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buffer) {
    Connection* connection = static_cast<Connection*>(stream->data);
    if (nread > 0) {
      connection->handle_data(reinterpret_cast<const std::uint8_t*>(buffer->base),
                              static_cast<std::size_t>(nread));
    }
    else if (nread < 0) {
      connection->begin_teardown();
    }
    if (buffer->base != nullptr) {
      std::free(buffer->base);
    }
  }

  void Connection::handle_data(const std::uint8_t* data, std::size_t len) {
    if (state_ == State::Handshaking) {
      handle_handshake_data(data, len);
    }
    else if (state_ == State::Streaming) {
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
    if (handshake_buffer_.size() < kHandshakeRequestSize) {
      return;
    }

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
      return;
    }

    integrity_checks_ = has_flag(request.flags, HandshakeFlag::IntegrityChecks);
    state_ = State::Streaming;

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

  void Connection::send_handshake_reply(HandshakeStatus status) {
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

      SequentialWriter sequential;
      OffsetWriter offset;
      if (type == UnitType::File) {
        sequential = sink_->open_sequential(path);
        if (!sequential.valid()) {
          break;
        }
      }
      else {
        offset = sink_->open_offset(path);
        if (!offset.valid()) {
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
          unit_ok = false;
        }
      }

      if (type == UnitType::File) {
        sequential.close();
      }

      if (!unit_ok || !unit_done) {
        break;
      }
    }

    uv_async_send(&close_async_);
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
      delete connection;
    }
  }

}
