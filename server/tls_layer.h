#pragma once

#include <cstdint>
#include <cstddef>

#include <openssl/ssl.h>

namespace dicker {
struct Connection;

struct TlsLayer {
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;
  BIO* rbio = nullptr;
  BIO* wbio = nullptr;
  bool handshake_done = false;

  ~TlsLayer();
  bool init();
  void ingest(Connection* connection, const std::uint8_t* data, std::size_t len);
  void flush_out(Connection* connection, bool teardown_after = false);
};
}
