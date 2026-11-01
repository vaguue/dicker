#pragma once

#include <cstddef>
#include <string>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <unistd.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
#endif

#include "log.h"
#include "net_io.h"

namespace dicker {
namespace net {

struct AddrInfoList {
  addrinfo* head = nullptr;

  AddrInfoList() = default;

  AddrInfoList(const AddrInfoList&) = delete;
  AddrInfoList& operator=(const AddrInfoList&) = delete;

  ~AddrInfoList() {
    if (this->head != nullptr) {
      freeaddrinfo(this->head);
    }
  }
};

struct Socket {
  socket_t fd = INVALID;

  Socket() = default;

  explicit Socket(socket_t fd) : fd{fd} {}

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  Socket(Socket&& other) noexcept : fd{other.release()} {}

  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      this->reset(other.release());
    }
    return *this;
  }

  ~Socket() {
    this->reset();
  }

  bool valid() const noexcept {
    return isValid(this->fd);
  }

  socket_t release() noexcept {
    const socket_t taken = this->fd;
    this->fd = INVALID;
    return taken;
  }

  void reset(socket_t next = INVALID) noexcept {
    if (isValid(this->fd)) {
      closeSock(this->fd);
    }
    this->fd = next;
  }
};

inline void shutdownWrite(socket_t fd) noexcept {
#ifdef _WIN32
  ::shutdown(fd, SD_SEND);
#else
  ::shutdown(fd, SHUT_WR);
#endif
}

inline void shutdownBoth(socket_t fd) noexcept {
#ifdef _WIN32
  ::shutdown(fd, SD_BOTH);
#else
  ::shutdown(fd, SHUT_RDWR);
#endif
}

inline void setAddressReuse(socket_t fd) noexcept {
  int enabled = 1;

#ifdef _WIN32
  ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#else
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
}

inline Socket tcpListen(const char* host, const char* port, int backlog = 512) {
  init();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  AddrInfoList resolved;

  if (getaddrinfo(host, port, &hints, &resolved.head) != 0 || resolved.head == nullptr) {
    log().error("listen resolve fail %s:%s", host, port);
    return Socket();
  }

  for (addrinfo* ai = resolved.head; ai != nullptr; ai = ai->ai_next) {
    Socket sock{::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)};

    if (!sock.valid()) {
      continue;
    }

    applyNoSigpipe(sock.fd);
    setAddressReuse(sock.fd);

    if (::bind(sock.fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) != 0) {
      continue;
    }
    if (::listen(sock.fd, backlog) != 0) {
      continue;
    }

    return sock;
  }

  log().error("listen fail %s:%s err=%d", host, port, lastError());

  return Socket();
}

inline Socket tcpAccept(socket_t listener, std::string* peer) {
  sockaddr_storage address{};
  socklen_t addressLength = sizeof(address);

  Socket accepted{::accept(listener, reinterpret_cast<sockaddr*>(&address), &addressLength)};

  if (!accepted.valid()) {
    return Socket();
  }

  applyNoSigpipe(accepted.fd);
  setNoDelay(accepted.fd, true);

  if (peer != nullptr) {
    char host[NI_MAXHOST] = {0};
    char service[NI_MAXSERV] = {0};

    if (getnameinfo(reinterpret_cast<sockaddr*>(&address), addressLength,
                    host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
      *peer = std::string(host) + ":" + service;
    }
  }

  return accepted;
}

}
}  // namespace dicker
