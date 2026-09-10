#pragma once

#include <array>
#include <string>
#include <random>
#include <cstdint>
#include <cstddef>

#include "protocol.h"

struct Proxy {
  bool use = false;
  std::string host, port, user, pass;
};

// 16 random bytes, stable for the life of one Conn (all its connections share it,
// so they land in the same server session dir). Override by setting sessionId.
inline std::array<std::uint8_t, 16> randomSessionId() {
  std::array<std::uint8_t, 16> id{};
  std::random_device rd;
  for (std::size_t i = 0; i < id.size(); i += 4) {
    std::uint32_t r = rd();
    id[i] = static_cast<std::uint8_t>(r);
    id[i + 1] = static_cast<std::uint8_t>(r >> 8);
    id[i + 2] = static_cast<std::uint8_t>(r >> 16);
    id[i + 3] = static_cast<std::uint8_t>(r >> 24);
  }
  return id;
}

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

// Built from a URL: Conn{"dicker://[sessionId:]secret@host:port?query"} with an
// optional second SOCKS5 proxy URL. Scheme and userinfo are optional. The query
// is &-separated named params: "integrity" (or integrity=1) sets the integrity
// bit; "compressionAlgo=zstd|lz4|none" (alias "algo") picks the codec. An
// explicit sessionId is raw bytes (<=16, zero-padded); otherwise it stays random.
struct Conn {
  std::string host;
  std::string port;
  std::array<uint8_t, 16> sessionId = randomSessionId();
  std::string key;
  uint8_t flags = 0;
  dicker::CompressionAlgo compressionAlgo = dicker::CompressionAlgo::Zstd;
  Proxy proxy = {};

  Conn() = default;

  explicit Conn(const std::string& url, const std::string& proxyUrl = "") {
    this->parse(url);
    if (!proxyUrl.empty()) {
      this->proxy = parseProxy(proxyUrl);
    }
  }

  void parse(const std::string& url) {
    std::string s = url;
    if (auto scheme = s.find("://"); scheme != std::string::npos) {
      s = s.substr(scheme + 3);
    }

    std::string query;
    if (auto q = s.find('?'); q != std::string::npos) {
      query = s.substr(q + 1);
      s = s.substr(0, q);
    }

    // userinfo before '@' is [sessionId:]secret, or just secret, or absent.
    std::string hostport = s;
    if (auto at = s.rfind('@'); at != std::string::npos) {
      std::string userinfo = s.substr(0, at);
      hostport = s.substr(at + 1);

      if (auto colon = userinfo.find(':'); colon != std::string::npos) {
        std::string sid = userinfo.substr(0, colon);
        this->key = userinfo.substr(colon + 1);

        if (!sid.empty()) {
          this->sessionId = {};   // explicit id: raw bytes, up to 16, zero-padded
          for (std::size_t i = 0; i < sid.size() && i < this->sessionId.size(); i += 1) {
            this->sessionId[i] = static_cast<std::uint8_t>(sid[i]);
          }
        }
      }
      else {
        this->key = userinfo;
      }
    }

    if (auto colon = hostport.rfind(':'); colon != std::string::npos) {
      this->host = hostport.substr(0, colon);
      this->port = hostport.substr(colon + 1);
    }
    else {
      this->host = hostport;
    }

    std::size_t start = 0;
    while (start < query.size()) {
      std::size_t amp = query.find('&', start);
      std::string token = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);

      std::string k = token;
      std::string v;
      if (auto eq = token.find('='); eq != std::string::npos) {
        k = token.substr(0, eq);
        v = token.substr(eq + 1);
      }

      if (k == "integrity") {
        if (v.empty() || v == "1" || v == "true") {
          this->flags |= static_cast<std::uint8_t>(dicker::HandshakeFlag::IntegrityChecks);
        }
      }
      else if (k == "compressionAlgo" || k == "algo") {
        if (v == "zstd") {
          this->compressionAlgo = dicker::CompressionAlgo::Zstd;
        }
        else if (v == "lz4") {
          this->compressionAlgo = dicker::CompressionAlgo::Lz4;
        }
        else if (v == "none") {
          this->compressionAlgo = dicker::CompressionAlgo::None;
        }
      }

      if (amp == std::string::npos) {
        break;
      }
      start = amp + 1;
    }
  }
};
