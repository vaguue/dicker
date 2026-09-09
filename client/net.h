#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <mutex>

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

namespace net {

#ifdef _WIN32
  using socket_t = SOCKET;
  static const socket_t INVALID = INVALID_SOCKET;
  inline bool isValid(socket_t s) { return s != INVALID_SOCKET; }
  inline void closeSock(socket_t s) { if (isValid(s)) ::closesocket(s); }
  inline int  lastError() { return WSAGetLastError(); }
  inline void init() {
    static std::once_flag once;
    std::call_once(once, [] { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); });
  }
#else
  using socket_t = int;
  static const socket_t INVALID = -1;
  inline bool isValid(socket_t s) { return s >= 0; }
  inline void closeSock(socket_t s) { if (isValid(s)) ::close(s); }
  inline int  lastError() { return errno; }
  inline void init() {}
#endif

// send()/recv() differ in buffer type (void* vs char*) and length type across platforms — wrap them.
// On Linux MSG_NOSIGNAL suppresses SIGPIPE on a write to a peer-closed socket (would kill the
// process otherwise); macOS/BSD have no such flag, we set SO_NOSIGPIPE on the socket instead.
inline int tcpSend(socket_t s, const void* buf, size_t len, int flags) {
#ifdef _WIN32
  return ::send(s, (const char*)buf, (int)len, flags);
#elif defined(__linux__)
  return (int)::send(s, buf, len, flags | MSG_NOSIGNAL);
#else
  return (int)::send(s, buf, len, flags);
#endif
}
inline int tcpRecv(socket_t s, void* buf, size_t len, int flags) {
#ifdef _WIN32
  return ::recv(s, (char*)buf, (int)len, flags);
#else
  return (int)::recv(s, buf, len, flags);
#endif
}

inline void setRecvTimeout(socket_t s, int secs) {
#ifdef _WIN32
  DWORD ms = (DWORD)secs * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
#else
  struct timeval tv { secs, 0 };
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

// disable Nagle: send small writes immediately instead of coalescing (~40ms). Latency win for
// chatty small messages; no benefit for bulk transfers.
inline void setNoDelay(socket_t s, bool on = true) {
  int v = on ? 1 : 0;
#ifdef _WIN32
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&v, sizeof v);
#else
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
#endif
}

// macOS/BSD: suppress SIGPIPE at the socket level (Linux uses MSG_NOSIGNAL in tcpSend, Windows n/a).
inline void applyNoSigpipe(socket_t s) {
#if defined(__APPLE__)
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
  (void)s;
#endif
}

inline void setBlocking(socket_t s, bool blocking) {
#ifdef _WIN32
  u_long mode = blocking ? 0 : 1;
  ioctlsocket(s, FIONBIO, &mode);
#else
  int fl = fcntl(s, F_GETFL, 0);
  if (fl < 0) {
    return;
  }
  fcntl(s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

// ── low-level helpers ──────────────────────────────────────────────────────────────────────────
inline bool sendAll(socket_t s, const void* buf, size_t len) {
  const uint8_t* p = (const uint8_t*)buf;
  size_t off = 0;

  while (off < len) {
    int w = tcpSend(s, p + off, len - off, 0);

    if (w <= 0) {
      return false;
    }
    off += (size_t)w;
  }
  return true;
}

inline bool recvExact(socket_t s, void* buf, size_t len) {
  uint8_t* p = (uint8_t*)buf;
  size_t off = 0;

  while (off < len) {
    int r = tcpRecv(s, p + off, len - off, 0);

    if (r <= 0) {
      return false;
    }
    off += (size_t)r;
  }

  return true;
}

// non-blocking connect with a bounded wait. A blocking connect() to a dead host hangs on the OS
// default (~75s on Linux); this caps it at secs. Leaves the socket blocking on success.
inline bool connectTimeout(socket_t s, const sockaddr* addr, socklen_t len, int secs) {
  setBlocking(s, false);

  int rc = ::connect(s, addr, len);

  if (rc == 0) {
    setBlocking(s, true);
    return true;                                         // connected immediately (e.g. localhost)
  }

#ifdef _WIN32
  if (lastError() != WSAEWOULDBLOCK) {
#else
  if (lastError() != EINPROGRESS) {
#endif
    return false;
  }

  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(s, &wfds);
  struct timeval tv { secs, 0 };

  rc = select((int)s + 1, nullptr, &wfds, nullptr, &tv);

  if (rc <= 0) {
    return false;                                        // timeout (0) or select error (<0)
  }

  int err = 0;
  socklen_t elen = sizeof err;

#ifdef _WIN32
  getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
#else
  getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
#endif

  if (err != 0) {
    return false;                                        // connect failed (refused / unreachable)
  }

  setBlocking(s, true);
  return true;
}

// open a raw TCP connection to host:port (no proxy). Tries every resolved address (v4 and v6) with
// a per-attempt connect timeout. Returns net::INVALID on failure.
inline socket_t tcpDial(const char* host, const char* port, int timeoutSecs = 10, bool nodelay = false) {
  init();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
    std::cerr << "[!] DNS fail " << host << "\n";
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
      freeaddrinfo(res);
      return fd;
    }

    closeSock(fd);                                       // this address dead, try the next one
  }

  std::cerr << "[!] connect fail " << host << ":" << port << "\n";
  freeaddrinfo(res);
  return INVALID;
}

// SOCKS5 handshake over an already-connected proxy socket, CONNECTing to host:port (domain ATYP so
// the proxy resolves DNS). RFC 1928 + RFC 1929 (user/pass). Returns true on GRANTED tunnel.
inline bool socks5Connect(socket_t s, const Proxy& px, const char* host, const char* port) {
  // greeting: offer no-auth (0x00) and user/pass (0x02)
  uint8_t greet[4] = {0x05, 0x02, 0x00, 0x02};

  if (!sendAll(s, greet, sizeof greet)) {
    return false;
  }

  uint8_t sel[2];

  if (!recvExact(s, sel, 2) || sel[0] != 0x05) {
    return false;
  }

  if (sel[1] == 0x02) {                                  // username/password auth
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
      std::cerr << "[!] SOCKS5 auth rejected\n";
      return false;
    }
  }
  else if (sel[1] != 0x00) {                           // 0xFF = no acceptable methods
    std::cerr << "[!] SOCKS5 no acceptable auth method (0x" << std::hex << (int)sel[1] << std::dec << ")\n";
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
    std::cerr << "[!] SOCKS5 CONNECT failed (rep=0x" << std::hex << (int)rep[1] << std::dec << ")\n";
    return false;
  }

  size_t skip = 0;
  switch (rep[3]) {
    case 0x01: skip = 4; break;                          // IPv4
    case 0x04: skip = 16; break;                         // IPv6
    case 0x03: { uint8_t l; if (!recvExact(s, &l, 1)) return false; skip = l; break; }  // domain
    default: return false;
  }

  uint8_t drain[260];                                    // max domain 255 + 2-byte port

  if (!recvExact(s, drain, skip + 2)) {
    return false;      // bound addr + 2-byte port
  }

  return true;
}

inline socket_t tcpConnect(const char* host, const char* port, const Proxy& px = {},
                           int timeoutSecs = 10, bool nodelay = false) {
  if (!px.use) {
    return tcpDial(host, port, timeoutSecs, nodelay);
  }

  std::cerr << "[i] via SOCKS5 " << px.host << ":" << px.port << " -> " << host << ":" << port << "\n";

  socket_t s = tcpDial(px.host.c_str(), px.port.c_str(), timeoutSecs, nodelay);

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

  void init(const Conn& conn) {
    this->fd = net::tcpConnect(conn.host, conn.port, conn.proxy);

    if (!net::isValid(this->fd)) {
      throw std::runtime_error{"failed to connect to server"};
    }
  }

  void process(NetworkTask& t) {
    net::sendAll(this->fd, t.data.data(), t.data.size());
  }

  bool recvExact(void* buf, size_t len) {
    return net::recvExact(this->fd, buf, len);
  }
};
