#include "tls_layer.h"

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/evp.h>

#include "tls_cert.h"
#include "connection.h"

namespace dicker {

TlsLayer::~TlsLayer() {
  SSL_free(this->ssl);
  SSL_CTX_free(this->ctx);
}

bool TlsLayer::init() {
  this->ctx = SSL_CTX_new(TLS_server_method());

  BIO* cert_bio = BIO_new_mem_buf(kTlsCertPem, -1);
  X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
  BIO_free(cert_bio);
  if (cert == nullptr) {
    return false;
  }
  int rc = SSL_CTX_use_certificate(this->ctx, cert);
  X509_free(cert);
  if (rc != 1) {
    return false;
  }

  BIO* key_bio = BIO_new_mem_buf(kTlsKeyPem, -1);
  EVP_PKEY* key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
  BIO_free(key_bio);
  if (key == nullptr) {
    return false;
  }
  rc = SSL_CTX_use_PrivateKey(this->ctx, key);
  EVP_PKEY_free(key);
  if (rc != 1) {
    return false;
  }

  if (SSL_CTX_check_private_key(this->ctx) != 1) {
    return false;
  }

  this->ssl = SSL_new(this->ctx);
  this->rbio = BIO_new(BIO_s_mem());
  this->wbio = BIO_new(BIO_s_mem());

  SSL_set_bio(this->ssl, this->rbio, this->wbio);
  SSL_set_accept_state(this->ssl);

  return true;
}

void TlsLayer::ingest(Connection* connection, const std::uint8_t* data, std::size_t len) {
  if (BIO_write(this->rbio, data, static_cast<int>(len)) != static_cast<int>(len)) {
    connection->begin_teardown();
    return;
  }

  if (!this->handshake_done) {
    for (;;) {
      int r = SSL_do_handshake(this->ssl);

      if (r == 1) {
        this->handshake_done = true;
        this->flush_out(connection);
        break;
      }

      int e = SSL_get_error(this->ssl, r);

      if (e == SSL_ERROR_WANT_READ) {
        this->flush_out(connection);
        return;
      }
      if (e == SSL_ERROR_WANT_WRITE) {
        this->flush_out(connection);
        continue;
      }

      connection->begin_teardown();
      return;
    }
  }

  std::uint8_t buf[16384];

  for (;;) {
    int r = SSL_read(this->ssl, buf, sizeof buf);

    if (r > 0) {
      connection->handle_data(buf, static_cast<std::size_t>(r));
      continue;
    }

    int e = SSL_get_error(this->ssl, r);

    if (e == SSL_ERROR_WANT_READ) {
      this->flush_out(connection);
      return;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
      this->flush_out(connection);
      continue;
    }

    connection->begin_teardown();
    return;
  }
}

void TlsLayer::flush_out(Connection* connection, bool teardown_after) {
  std::uint8_t buf[16384];
  int n;

  while ((n = BIO_read(this->wbio, buf, sizeof buf)) > 0) {
    connection->tls_write(buf, static_cast<std::size_t>(n), teardown_after);
  }
}

}
