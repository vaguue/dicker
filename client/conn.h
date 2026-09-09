#pragma once

#include <array>
#include <string>
#include <cstdint>

struct Proxy {
  bool use = false;
  std::string host, port, user, pass;
};

// parse "socks5://user:pass@host:port" (auth optional). Returns {use=false} for an empty string.
inline Proxy parseProxy(const std::string& url) {
  Proxy p;

  if (url.empty()) {
    return p;
  }

  std::string s = url;

  auto scheme = s.find("://");

  if (scheme != std::string::npos) {
    s = s.substr(scheme + 3);   // drop "socks5://"
  }

  std::string auth, hostport;

  if (auto at = s.rfind('@'); at != std::string::npos) {
    auth = s.substr(0, at);
    hostport = s.substr(at + 1);
  }
  else {
    hostport = s;
  }

  if (!auth.empty()) {
    if (auto c = auth.find(':'); c != std::string::npos) {
      p.user = auth.substr(0, c);
      p.pass = auth.substr(c + 1);
    }
    else {
      p.user = auth;
    }
  }

  if (auto c = hostport.rfind(':'); c != std::string::npos) {
    p.host = hostport.substr(0, c);
    p.port = hostport.substr(c + 1);
  }
  else {
    p.host = hostport;
    p.port = "1080";
  }

  p.use = !p.host.empty();

  return p;
}

struct Conn {
  const char* host;
  const char* port;
  Proxy proxy;
  std::array<uint8_t, 16> sessionId;
  std::string key;
  uint8_t flags;
};
