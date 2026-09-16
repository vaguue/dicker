#include "tls_layer.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#include <wolfssl/ssl.h>
#include <wolfssl/wolfio.h>   // WOLFSSL_CBIO_ERR_* callback return codes

#include "tls_cert.h"
#include "connection.h"

namespace dicker {

namespace {
void tlsError(const char* what, int code) {
  char ebuf[WOLFSSL_MAX_ERROR_SZ];
  std::fprintf(stderr, "[tls] %s: %s\n", what,
               wolfSSL_ERR_error_string(static_cast<unsigned long>(code), ebuf));
}

// Custom I/O over TlsLayer's in-memory buffers. wolfSSL pulls ciphertext here
// (WANT_READ when the inbound buffer is drained) and pushes ciphertext here.
int tlsIoRecv(WOLFSSL* ssl, char* buf, int sz, void* vctx) {
  (void)ssl;
  TlsLayer* self = static_cast<TlsLayer*>(vctx);

  std::size_t avail = self->in_buf.size() - self->in_pos;
  if (avail == 0) {
    return WOLFSSL_CBIO_ERR_WANT_READ;
  }

  int n = sz;
  if (static_cast<std::size_t>(n) > avail) {
    n = static_cast<int>(avail);
  }
  std::memcpy(buf, self->in_buf.data() + self->in_pos, static_cast<std::size_t>(n));
  self->in_pos += static_cast<std::size_t>(n);
  return n;
}

int tlsIoSend(WOLFSSL* ssl, char* buf, int sz, void* vctx) {
  (void)ssl;
  TlsLayer* self = static_cast<TlsLayer*>(vctx);

  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(buf);
  self->out_buf.insert(self->out_buf.end(), p, p + sz);
  return sz;
}
}

TlsLayer::~TlsLayer() {
  if (this->ssl != nullptr) {
    wolfSSL_free(this->ssl);
  }
  if (this->ctx != nullptr) {
    wolfSSL_CTX_free(this->ctx);
  }
}

bool TlsLayer::init() {
  // Thread-safe one-time library init (C++11 magic static).
  static const int wolfInit = wolfSSL_Init();
  (void)wolfInit;

  this->ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
  if (this->ctx == nullptr) {
    std::fprintf(stderr, "[tls] wolfSSL_CTX_new failed\n");
    return false;
  }

  int rc = wolfSSL_CTX_use_certificate_buffer(
      this->ctx, reinterpret_cast<const unsigned char*>(kTlsCertPem),
      static_cast<long>(std::strlen(kTlsCertPem)), WOLFSSL_FILETYPE_PEM);
  if (rc != WOLFSSL_SUCCESS) {
    tlsError("use_certificate_buffer failed", rc);
    return false;
  }

  rc = wolfSSL_CTX_use_PrivateKey_buffer(
      this->ctx, reinterpret_cast<const unsigned char*>(kTlsKeyPem),
      static_cast<long>(std::strlen(kTlsKeyPem)), WOLFSSL_FILETYPE_PEM);
  if (rc != WOLFSSL_SUCCESS) {
    tlsError("use_PrivateKey_buffer failed", rc);
    return false;
  }

  wolfSSL_CTX_SetIORecv(this->ctx, &tlsIoRecv);
  wolfSSL_CTX_SetIOSend(this->ctx, &tlsIoSend);

  this->ssl = wolfSSL_new(this->ctx);
  if (this->ssl == nullptr) {
    std::fprintf(stderr, "[tls] wolfSSL_new failed\n");
    return false;
  }
  wolfSSL_SetIOReadCtx(this->ssl, this);
  wolfSSL_SetIOWriteCtx(this->ssl, this);
  // A WOLFSSL built from a *_server_method CTX is already server-side; wolfSSL_accept
  // drives the handshake. (wolfSSL_set_accept_state is OpenSSL-compat / opensslextra.)

  return true;
}

void TlsLayer::ingest(Connection* connection, const std::uint8_t* data, std::size_t len) {
  // Drop the already-consumed prefix, then append the new ciphertext.
  if (this->in_pos > 0) {
    this->in_buf.erase(this->in_buf.begin(), this->in_buf.begin() + this->in_pos);
    this->in_pos = 0;
  }
  this->in_buf.insert(this->in_buf.end(), data, data + len);

  if (!this->handshake_done) {
    for (;;) {
      int r = wolfSSL_accept(this->ssl);

      if (r == WOLFSSL_SUCCESS) {
        this->handshake_done = true;
        this->flush_out(connection);
        break;
      }

      int e = wolfSSL_get_error(this->ssl, r);

      if (e == WOLFSSL_ERROR_WANT_READ) {
        this->flush_out(connection);
        return;
      }
      if (e == WOLFSSL_ERROR_WANT_WRITE) {
        this->flush_out(connection);
        continue;
      }

      connection->begin_teardown();
      return;
    }
  }

  std::uint8_t buf[16384];

  for (;;) {
    int r = wolfSSL_read(this->ssl, buf, sizeof buf);

    if (r > 0) {
      connection->handle_data(buf, static_cast<std::size_t>(r));
      continue;
    }

    int e = wolfSSL_get_error(this->ssl, r);

    if (e == WOLFSSL_ERROR_WANT_READ) {
      this->flush_out(connection);
      return;
    }
    if (e == WOLFSSL_ERROR_WANT_WRITE) {
      this->flush_out(connection);
      continue;
    }

    connection->begin_teardown();
    return;
  }
}

bool TlsLayer::send(Connection* connection, const std::uint8_t* data, std::size_t len,
                    bool teardown_after) {
  if (wolfSSL_write(this->ssl, data, static_cast<int>(len)) <= 0) {
    return false;
  }
  this->flush_out(connection, teardown_after);
  return true;
}

void TlsLayer::flush_out(Connection* connection, bool teardown_after) {
  std::size_t off = 0;

  while (off < this->out_buf.size()) {
    std::size_t n = std::min<std::size_t>(16384, this->out_buf.size() - off);
    connection->tls_write(this->out_buf.data() + off, n, teardown_after);
    off += n;
  }

  this->out_buf.clear();
}

}
