#pragma once
#include <cstdint>
#include <cstddef>
#include <mutex>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <cerrno>
#endif

#include "log.h"

namespace dicker {
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

inline void setSendTimeout(socket_t s, int secs) {
#ifdef _WIN32
  DWORD ms = (DWORD)secs * 1000;
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof ms);
#else
  struct timeval tv { secs, 0 };
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
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

inline bool setBlocking(socket_t s, bool blocking) {
#ifdef _WIN32
  u_long mode = blocking ? 0 : 1;
  if (ioctlsocket(s, FIONBIO, &mode) == SOCKET_ERROR) {
    log().error("ioctlsocket failed, err=%d", WSAGetLastError());
    return false;
  }
#else
  int fl = fcntl(s, F_GETFL, 0);
  if (fl < 0) return false;
  if (fcntl(s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK)) < 0) {
    log().error("fcntl failed, err=%d", errno);
    return false;
  }
#endif
  return true;
}

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

inline bool connectTimeout(socket_t s, const sockaddr* addr, socklen_t len, int secs) {
  setBlocking(s, false);

  int rc = ::connect(s, addr, len);

  if (rc == 0) {
    setBlocking(s, true);
    return true;
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

#ifdef _WIN32
  rc = select(0, nullptr, &wfds, nullptr, &tv);
#else
  rc = select((int)s + 1, nullptr, &wfds, nullptr, &tv);
#endif

  if (rc <= 0) {
    return false;
  }

  int err = 0;
  socklen_t elen = sizeof err;

#ifdef _WIN32
  getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
#else
  getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
#endif

  if (err != 0) {
    return false;
  }

  setBlocking(s, true);
  return true;
}

}
}
