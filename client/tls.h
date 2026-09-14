#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

#include <openssl/ssl.h>

#include "net_io.h"

namespace dicker {
struct TlsSession {
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;
  BIO* rbio = nullptr;
  BIO* wbio = nullptr;
  net::socket_t fd = net::INVALID;

  TlsSession() = default;
  TlsSession(const TlsSession&) = delete;
  TlsSession& operator=(const TlsSession&) = delete;

  ~TlsSession() {
    SSL_free(this->ssl);
    SSL_CTX_free(this->ctx);
  }

  bool pumpOut() {
    uint8_t buf[16384];
    int n;

    while ((n = BIO_read(this->wbio, buf, sizeof buf)) > 0) {
      if (!net::sendAll(this->fd, buf, static_cast<size_t>(n))) {
        return false;
      }
    }

    return true;
  }

  bool pumpIn() {
    uint8_t buf[16384];
    int n = net::tcpRecv(this->fd, buf, sizeof buf, 0);

    if (n <= 0) {
      return false;
    }

    return BIO_write(this->rbio, buf, n) > 0;
  }

  bool handshake(net::socket_t fd, const std::string& host) {
    this->fd = fd;

    this->ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(this->ctx, SSL_VERIFY_NONE, nullptr);

    this->ssl = SSL_new(this->ctx);
    this->rbio = BIO_new(BIO_s_mem());
    this->wbio = BIO_new(BIO_s_mem());

    SSL_set_bio(this->ssl, this->rbio, this->wbio);
    SSL_set_tlsext_host_name(this->ssl, host.c_str());
    SSL_set_connect_state(this->ssl);

    for (;;) {
      int r = SSL_do_handshake(this->ssl);

      if (!this->pumpOut()) {
        return false;
      }

      if (r == 1) {
        return true;
      }

      int e = SSL_get_error(this->ssl, r);

      if (e == SSL_ERROR_WANT_READ) {
        if (!this->pumpIn()) {
          return false;
        }
      }
      else if (e == SSL_ERROR_WANT_WRITE) {
        continue;
      }
      else {
        return false;
      }
    }
  }

  bool sendAll(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t off = 0;

    while (off < len) {
      int n = SSL_write(this->ssl, p + off, static_cast<int>(len - off));

      if (n > 0) {
        off += static_cast<size_t>(n);
        continue;
      }

      int e = SSL_get_error(this->ssl, n);

      if (e == SSL_ERROR_WANT_READ) {
        if (!this->pumpIn()) {
          return false;
        }
      }
      else if (e == SSL_ERROR_WANT_WRITE) {
        if (!this->pumpOut()) {
          return false;
        }
      }
      else {
        return false;
      }
    }

    return this->pumpOut();
  }

  bool recvExact(void* out, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(out);
    size_t off = 0;

    while (off < len) {
      int n = SSL_read(this->ssl, p + off, static_cast<int>(len - off));

      if (n > 0) {
        off += static_cast<size_t>(n);
        continue;
      }

      int e = SSL_get_error(this->ssl, n);

      if (e == SSL_ERROR_WANT_READ) {
        if (!this->pumpIn()) {
          return false;
        }
      }
      else if (e == SSL_ERROR_WANT_WRITE) {
        if (!this->pumpOut()) {
          return false;
        }
      }
      else {
        return false;
      }
    }

    return true;
  }
};
}  // namespace dicker
