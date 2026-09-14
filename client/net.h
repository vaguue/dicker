#pragma once
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <unistd.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <cerrno>
#endif

#include "conn.h"
#include "chan.h"
#include "task.h"
#include "log.h"
#include "net_io.h"
#include "tls.h"

namespace dicker {
namespace net {

inline socket_t tcpDial(const char* host, const char* port, int timeoutSecs = 10, bool nodelay = false,
                        int recvTimeoutSecs = 10, int sendTimeoutSecs = 60) {
  init();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
    log().error("DNS fail %s", host);
    return INVALID;
  }

  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    socket_t fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

    if (!isValid(fd)) {
      continue;
    }

    applyNoSigpipe(fd);

    if (nodelay) {
      setNoDelay(fd, true);
    }

    if (connectTimeout(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen, timeoutSecs)) {
      setRecvTimeout(fd, recvTimeoutSecs);
      setSendTimeout(fd, sendTimeoutSecs);
      freeaddrinfo(res);
      return fd;
    }

    closeSock(fd);
  }

  log().error("connect fail %s:%s", host, port);
  freeaddrinfo(res);
  return INVALID;
}

inline bool socks5Connect(socket_t s, const Proxy& px, const char* host, const char* port) {
  uint8_t greet[4] = {0x05, 0x02, 0x00, 0x02};

  if (!sendAll(s, greet, sizeof greet)) {
    return false;
  }

  uint8_t sel[2];

  if (!recvExact(s, sel, 2) || sel[0] != 0x05) {
    return false;
  }

  if (sel[1] == 0x02) { // username/password auth
    std::vector<uint8_t> a;

    a.push_back(0x01);
    a.push_back((uint8_t)px.user.size());
    a.insert(a.end(), px.user.begin(), px.user.end());
    a.push_back((uint8_t)px.pass.size());
    a.insert(a.end(), px.pass.begin(), px.pass.end());

    if (!sendAll(s, a.data(), a.size())) {
      return false;
    }

    uint8_t ar[2];

    if (!recvExact(s, ar, 2) || ar[1] != 0x00) {
      log().error("SOCKS5 auth rejected");
      return false;
    }
  }
  else if (sel[1] != 0x00) { // 0xFF = no acceptable methods
    log().error("SOCKS5 no acceptable auth method (0x%02x)", (int)sel[1]);
    return false;
  }

  // CONNECT request with domain address
  size_t hl = std::strlen(host);

  if (hl > 255) {
    return false;
  }

  uint16_t p16 = (uint16_t)std::atoi(port);
  std::vector<uint8_t> req = {0x05, 0x01, 0x00, 0x03, (uint8_t)hl};
  req.insert(req.end(), host, host + hl);
  req.push_back((uint8_t)(p16 >> 8));
  req.push_back((uint8_t)(p16 & 0xff));

  if (!sendAll(s, req.data(), req.size())) {
    return false;
  }

  // reply: [ver rep rsv atyp] + bound addr + port
  uint8_t rep[4];

  if (!recvExact(s, rep, 4)) {
    return false;
  }

  if (rep[1] != 0x00) {
    log().error("SOCKS5 CONNECT failed (rep=0x%02x)", (int)rep[1]);
    return false;
  }

  size_t skip = 0;
  switch (rep[3]) {
    case 0x01: skip = 4; break; // IPv4
    case 0x04: skip = 16; break; // IPv6
    case 0x03: { uint8_t l; if (!recvExact(s, &l, 1)) return false; skip = l; break; } // domain
    default: return false;
  }

  uint8_t drain[260]; // max domain 255 + 2-byte port

  if (!recvExact(s, drain, skip + 2)) {
    return false; // bound addr + 2-byte port
  }

  return true;
}

inline socket_t tcpConnect(const char* host, const char* port, const Proxy& px = {},
                           int timeoutSecs = 10, bool nodelay = false, int recvTimeoutSecs = 10,
                           int sendTimeoutSecs = 60) {
  if (!px.use) {
    return tcpDial(host, port, timeoutSecs, nodelay, recvTimeoutSecs, sendTimeoutSecs);
  }

  log().info("via SOCKS5 %s:%s -> %s:%s", px.host.c_str(), px.port.c_str(), host, port);

  socket_t s = tcpDial(px.host.c_str(), px.port.c_str(), timeoutSecs, nodelay,
                       recvTimeoutSecs, sendTimeoutSecs);

  if (!isValid(s)) {
    return INVALID;
  }
  if (!socks5Connect(s, px, host, port)) {
    closeSock(s);
    return INVALID;
  }
  return s;
}

}

struct NetworkClient : Chan<NetworkClient, NetworkTask, 32> {
  net::socket_t fd = net::INVALID;
  bool logged_first_send = false;
  Conn conn;
  bool useTls = false;
  TlsSession tls;

  NetworkClient(Conn conn) : conn{conn}, useTls{conn.tls} {}

  bool init() noexcept {
    this->fd = net::tcpConnect(this->conn.host.c_str(), this->conn.port.c_str(),
                               this->conn.proxy, /*timeoutSecs=*/10, /*nodelay=*/true,
                               /*recvTimeoutSecs=*/10, /*sendTimeoutSecs=*/60);

    if (!net::isValid(this->fd)) {
      log().error("failed to connect to server");
      return false;
    }

    if (this->useTls && !this->tls.handshake(this->fd, this->conn.host)) {
      log().error("TLS handshake failed");
      net::closeSock(this->fd);
      this->fd = net::INVALID;
      return false;
    }

    return true;
  }

  ~NetworkClient() {
    if (net::isValid(this->fd)) {
      net::closeSock(this->fd);
      this->fd = net::INVALID;
    }
  }

  bool process(NetworkTask& t) noexcept {
    if (!this->logged_first_send) {
      this->logged_first_send = true;
      log().info("net thread sending first %zu bytes", t.data.size());
    }

    bool sent = this->useTls
      ? this->tls.sendAll(t.data.data(), t.data.size())
      : net::sendAll(this->fd, t.data.data(), t.data.size());

    if (!sent) {
      log().error("sendAll failed: fd=%lld err=%d", (long long)this->fd, net::lastError());
      net::closeSock(this->fd);
      this->fd = net::INVALID;
      return false;
    }

    return true;
  }

  bool recvExact(void* buf, size_t len) {
    if (this->useTls) {
      return this->tls.recvExact(buf, len);
    }
    return net::recvExact(this->fd, buf, len);
  }
};
}  // namespace dicker
