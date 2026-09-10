#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <uv.h>
#include "protocol.h"
#include "byte_channel.h"
#include "decompressor.h"

namespace dicker {

  struct Server;
  struct FileSink;

  struct Connection {
    static void accept(Server* server, uv_loop_t* loop, uv_stream_t* listener);

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    enum struct State {
      Handshaking,
      Streaming,
      Closing,
    };

    explicit Connection(Server* server);

    static void alloc_cb(uv_handle_t* handle, std::size_t suggested_size, uv_buf_t* buffer);
    static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buffer);
    static void on_write_reply(uv_write_t* request, int status);
    static void on_resume_async(uv_async_t* handle);
    static void on_close_async(uv_async_t* handle);
    static void on_handle_closed(uv_handle_t* handle);

    void handle_data(const std::uint8_t* data, std::size_t len);
    void handle_handshake_data(const std::uint8_t* data, std::size_t len);
    void send_handshake_reply(HandshakeStatus status);
    void apply_backpressure();
    void consumer_main();
    void begin_teardown();
    void finalize_close();
    void close_handle(uv_handle_t* handle);

    Server* server_;
    uv_tcp_t tcp_;
    uv_async_t resume_async_;
    uv_async_t close_async_;

    State state_;
    std::vector<std::uint8_t> handshake_buffer_;

    ByteChannel channel_;
    std::thread consumer_;
    std::unique_ptr<StreamDecompressor> decompressor_;
    std::shared_ptr<FileSink> sink_;
    bool integrity_checks_;

    std::string peer_;          // logging: "ip:port" of the client
    std::string session_hex_;   // logging: session id once handshaken
    std::uint64_t units_done_;  // logging: units fully received
    std::uint64_t bytes_total_; // logging: decompressed bytes written

    bool paused_;
    bool consumer_started_;
    bool teardown_started_;
    bool handles_closing_;
    int pending_handle_closes_;
  };

}
