#include "db.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

#include <fcntl.h>
#include <unistd.h>

#include <wolfssl/options.h>  // must precede other wolfssl headers
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>

#include "sha256.h"
#include "hmac_sha256.h"  // crypto::constant_time_equal

namespace dicker {

  namespace {

    constexpr std::size_t kNonceSize = 12;
    constexpr std::size_t kTagSize = 16;
    constexpr std::size_t kKeySize = 32;

    void derive_key(const std::string& secret, std::uint8_t out[kKeySize]) {
      crypto::sha256(reinterpret_cast<const std::uint8_t*>(secret.data()),
                     secret.size(), out);
    }

    bool gcm_seal(const std::uint8_t key[kKeySize],
                  const std::vector<std::uint8_t>& plain,
                  std::vector<std::uint8_t>& out) {
      std::uint8_t nonce[kNonceSize];
      WC_RNG rng;
      if (wc_InitRng(&rng) != 0) {
        return false;
      }
      int rc = wc_RNG_GenerateBlock(&rng, nonce, kNonceSize);
      wc_FreeRng(&rng);
      if (rc != 0) {
        return false;
      }

      Aes aes;
      if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) {
        return false;
      }
      if (wc_AesGcmSetKey(&aes, key, kKeySize) != 0) {
        wc_AesFree(&aes);
        return false;
      }

      std::vector<std::uint8_t> cipher(plain.size());
      std::uint8_t tag[kTagSize];
      rc = wc_AesGcmEncrypt(&aes, cipher.data(), plain.data(),
                            static_cast<word32>(plain.size()), nonce, kNonceSize,
                            tag, kTagSize, nullptr, 0);
      wc_AesFree(&aes);
      if (rc != 0) {
        return false;
      }

      out.clear();
      out.reserve(kNonceSize + kTagSize + cipher.size());
      out.insert(out.end(), nonce, nonce + kNonceSize);
      out.insert(out.end(), tag, tag + kTagSize);
      out.insert(out.end(), cipher.begin(), cipher.end());
      return true;
    }

    bool gcm_open(const std::uint8_t key[kKeySize],
                  const std::uint8_t* sealed, std::size_t sealed_len,
                  std::vector<std::uint8_t>& out) {
      if (sealed_len < kNonceSize + kTagSize) {
        return false;
      }
      const std::uint8_t* nonce = sealed;
      const std::uint8_t* tag = sealed + kNonceSize;
      const std::uint8_t* cipher = sealed + kNonceSize + kTagSize;
      std::size_t cipher_len = sealed_len - kNonceSize - kTagSize;

      Aes aes;
      if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) {
        return false;
      }
      if (wc_AesGcmSetKey(&aes, key, kKeySize) != 0) {
        wc_AesFree(&aes);
        return false;
      }

      out.resize(cipher_len);
      int rc = wc_AesGcmDecrypt(&aes, out.data(), cipher,
                                static_cast<word32>(cipher_len), nonce, kNonceSize,
                                tag, kTagSize, nullptr, 0);
      wc_AesFree(&aes);
      if (rc != 0) {
        out.clear();
        return false;
      }
      return true;
    }

    // ----- hex -----

    void hex_encode(const std::vector<std::uint8_t>& in, std::string& out) {
      static const char* digits = "0123456789abcdef";
      out.clear();
      out.reserve(in.size() * 2);
      for (std::uint8_t b : in) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0f]);
      }
    }

    bool hex_decode(const std::string& in, std::vector<std::uint8_t>& out) {
      if (in.size() % 2 != 0) {
        return false;
      }
      auto nibble = [](char c, int& v) -> bool {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        return false;
      };
      out.clear();
      out.reserve(in.size() / 2);
      for (std::size_t i = 0; i < in.size(); i += 2) {
        int hi, lo;
        if (!nibble(in[i], hi) || !nibble(in[i + 1], lo)) {
          return false;
        }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
      }
      return true;
    }

    // ----- minimal JSON -----

    void json_escape(const std::string& in, std::string& out) {
      out.push_back('"');
      for (unsigned char c : in) {
        switch (c) {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\b': out += "\\b"; break;
          case '\f': out += "\\f"; break;
          case '\n': out += "\\n"; break;
          case '\r': out += "\\r"; break;
          case '\t': out += "\\t"; break;
          default:
            if (c < 0x20) {
              char buf[8];
              std::snprintf(buf, sizeof(buf), "\\u%04x", c);
              out += buf;
            }
            else {
              out.push_back(static_cast<char>(c));
            }
        }
      }
      out.push_back('"');
    }

    struct JsonParser {
      const std::string& s;
      std::size_t i = 0;
      std::string error;

      explicit JsonParser(const std::string& text) : s(text) {}

      void skip_ws() {
        while (i < s.size()) {
          char c = s[i];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
          }
          else {
            break;
          }
        }
      }

      bool fail(const char* msg) {
        if (error.empty()) {
          error = msg;
        }
        return false;
      }

      bool expect(char c) {
        skip_ws();
        if (i >= s.size() || s[i] != c) {
          return fail("unexpected character");
        }
        ++i;
        return true;
      }

      bool peek(char c) {
        skip_ws();
        return i < s.size() && s[i] == c;
      }

      bool parse_string(std::string& out) {
        skip_ws();
        if (i >= s.size() || s[i] != '"') {
          return fail("expected string");
        }
        ++i;
        out.clear();
        while (i < s.size()) {
          char c = s[i++];
          if (c == '"') {
            return true;
          }
          if (c == '\\') {
            if (i >= s.size()) {
              return fail("bad escape");
            }
            char e = s[i++];
            switch (e) {
              case '"': out.push_back('"'); break;
              case '\\': out.push_back('\\'); break;
              case '/': out.push_back('/'); break;
              case 'b': out.push_back('\b'); break;
              case 'f': out.push_back('\f'); break;
              case 'n': out.push_back('\n'); break;
              case 'r': out.push_back('\r'); break;
              case 't': out.push_back('\t'); break;
              case 'u': {
                if (i + 4 > s.size()) {
                  return fail("bad \\u");
                }
                unsigned code = 0;
                for (int k = 0; k < 4; ++k) {
                  char h = s[i++];
                  code <<= 4;
                  if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                  else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                  else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                  else return fail("bad hex");
                }
                if (code < 0x80) {
                  out.push_back(static_cast<char>(code));
                }
                else if (code < 0x800) {
                  out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                  out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                else {
                  out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                  out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                  out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
              }
              default:
                return fail("bad escape");
            }
          }
          else {
            out.push_back(c);
          }
        }
        return fail("unterminated string");
      }

      bool parse_uint(std::uint64_t& out) {
        skip_ws();
        std::size_t start = i;
        std::uint64_t value = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
          value = value * 10 + static_cast<std::uint64_t>(s[i] - '0');
          ++i;
        }
        if (i == start) {
          return fail("expected number");
        }
        out = value;
        return true;
      }
    };

  }  // namespace

  // ---------------- TokenStore ----------------

  namespace {

    std::string tokens_to_json(const std::vector<std::string>& tokens) {
      std::string out = "{\"tokens\":[";
      for (std::size_t k = 0; k < tokens.size(); ++k) {
        if (k != 0) {
          out.push_back(',');
        }
        json_escape(tokens[k], out);
      }
      out += "]}";
      return out;
    }

    bool tokens_from_json(const std::string& text, std::vector<std::string>& out,
                          std::string& err) {
      JsonParser p(text);
      out.clear();
      if (!p.expect('{')) {
        err = p.error;
        return false;
      }
      if (p.peek('}')) {
        return true;
      }
      while (true) {
        std::string key;
        if (!p.parse_string(key) || !p.expect(':')) {
          err = p.error;
          return false;
        }
        if (key == "tokens") {
          if (!p.expect('[')) {
            err = p.error;
            return false;
          }
          if (!p.peek(']')) {
            while (true) {
              std::string tok;
              if (!p.parse_string(tok)) {
                err = p.error;
                return false;
              }
              out.push_back(std::move(tok));
              if (p.peek(',')) {
                ++p.i;
                continue;
              }
              break;
            }
          }
          if (!p.expect(']')) {
            err = p.error;
            return false;
          }
        }
        else {
          err = "unexpected key";
          return false;
        }
        if (p.peek(',')) {
          ++p.i;
          continue;
        }
        break;
      }
      if (!p.expect('}')) {
        err = p.error;
        return false;
      }
      return true;
    }

    std::vector<std::uint8_t> read_file(const std::string& path, bool& missing) {
      missing = false;
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        missing = true;
        return {};
      }
      return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                       std::istreambuf_iterator<char>());
    }

    // Whole-file atomic replace: unique temp -> fsync -> rename -> fsync(dir).
    bool write_file_atomic(const std::string& path,
                           const std::vector<std::uint8_t>& bytes, std::string& err) {
      std::string tmp = path + ".tmp." + std::to_string(::getpid());

      int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0) {
        err = "open temp failed";
        return false;
      }
      std::size_t off = 0;
      while (off < bytes.size()) {
        ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (n < 0) {
          ::close(fd);
          ::unlink(tmp.c_str());
          err = "write temp failed";
          return false;
        }
        off += static_cast<std::size_t>(n);
      }
      if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        err = "fsync temp failed";
        return false;
      }
      ::close(fd);

      if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        err = "rename failed";
        return false;
      }

      // Best-effort durability of the rename itself.
      std::filesystem::path parent = std::filesystem::path(path).parent_path();
      if (parent.empty()) {
        parent = ".";
      }
      int dfd = ::open(parent.c_str(), O_RDONLY);
      if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
      }
      return true;
    }

  }  // namespace

  bool TokenStore::load(const std::string& path, const std::string& key,
                        TokenStore& out, std::string& err) {
    out.tokens.clear();

    bool missing = false;
    std::vector<std::uint8_t> sealed = read_file(path, missing);
    if (missing || sealed.empty()) {
      return true;  // fresh, empty store
    }

    std::uint8_t derived[kKeySize];
    derive_key(key, derived);

    std::vector<std::uint8_t> plain;
    if (!gcm_open(derived, sealed.data(), sealed.size(), plain)) {
      err = "decrypt failed (wrong DICKER_KEY or corrupt token file)";
      return false;
    }

    std::string text(plain.begin(), plain.end());
    return tokens_from_json(text, out.tokens, err);
  }

  bool TokenStore::save(const std::string& path, const std::string& key,
                        std::string& err) const {
    std::string text = tokens_to_json(tokens);
    std::vector<std::uint8_t> plain(text.begin(), text.end());

    std::uint8_t derived[kKeySize];
    derive_key(key, derived);

    std::vector<std::uint8_t> sealed;
    if (!gcm_seal(derived, plain, sealed)) {
      err = "encrypt failed";
      return false;
    }
    return write_file_atomic(path, sealed, err);
  }

  bool TokenStore::verify(const std::string& token) const {
    bool matched = false;
    for (const std::string& candidate : tokens) {
      if (candidate.size() == token.size() &&
          crypto::constant_time_equal(
              reinterpret_cast<const std::uint8_t*>(candidate.data()),
              reinterpret_cast<const std::uint8_t*>(token.data()),
              token.size())) {
        matched = true;
      }
    }
    return matched;
  }

  // ---------------- SessionLog ----------------

  namespace {

    std::string session_row_to_json(const std::string& session_id,
                                    const std::string& ip, std::uint64_t created) {
      std::string out = "{\"sessionId\":";
      json_escape(session_id, out);
      out += ",\"ip\":";
      json_escape(ip, out);
      out += ",\"created\":";
      out += std::to_string(created);
      out += "}";
      return out;
    }

    bool session_row_from_json(const std::string& text, std::string& session_id,
                               SessionInfo& info) {
      JsonParser p(text);
      if (!p.expect('{')) {
        return false;
      }
      if (p.peek('}')) {
        return false;  // need at least sessionId
      }
      bool have_id = false;
      while (true) {
        std::string key;
        if (!p.parse_string(key) || !p.expect(':')) {
          return false;
        }
        if (key == "sessionId") {
          if (!p.parse_string(session_id)) {
            return false;
          }
          have_id = true;
        }
        else if (key == "ip") {
          if (!p.parse_string(info.ip)) {
            return false;
          }
        }
        else if (key == "created") {
          if (!p.parse_uint(info.created)) {
            return false;
          }
        }
        else {
          return false;
        }
        if (p.peek(',')) {
          ++p.i;
          continue;
        }
        break;
      }
      if (!p.expect('}')) {
        return false;
      }
      return have_id;
    }

  }  // namespace

  bool SessionLog::append(const std::string& path, const std::string& key,
                          const std::string& session_id, const std::string& ip,
                          std::uint64_t created, std::string& err) {
    std::string row = session_row_to_json(session_id, ip, created);
    std::vector<std::uint8_t> plain(row.begin(), row.end());

    std::uint8_t derived[kKeySize];
    derive_key(key, derived);

    std::vector<std::uint8_t> sealed;
    if (!gcm_seal(derived, plain, sealed)) {
      err = "encrypt failed";
      return false;
    }

    std::string line;
    hex_encode(sealed, line);
    line.push_back('\n');

    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) {
      err = "open sessions log failed";
      return false;
    }
    // One write() of a sub-page line: atomic against other appenders, and never
    // interleaved. (There is only one writer here anyway.)
    ssize_t n = ::write(fd, line.data(), line.size());
    if (n < 0 || static_cast<std::size_t>(n) != line.size()) {
      ::close(fd);
      err = "append sessions log failed";
      return false;
    }
    ::fsync(fd);
    ::close(fd);
    return true;
  }

  bool SessionLog::load_all(const std::string& path, const std::string& key,
                            std::map<std::string, SessionInfo>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return true;  // no log yet: no sessions
    }

    std::uint8_t derived[kKeySize];
    derive_key(key, derived);

    std::string line;
    while (std::getline(file, line)) {
      if (line.empty()) {
        continue;
      }
      std::vector<std::uint8_t> sealed;
      if (!hex_decode(line, sealed)) {
        continue;  // torn/garbage line
      }
      std::vector<std::uint8_t> plain;
      if (!gcm_open(derived, sealed.data(), sealed.size(), plain)) {
        continue;
      }
      std::string text(plain.begin(), plain.end());
      std::string session_id;
      SessionInfo info;
      if (session_row_from_json(text, session_id, info)) {
        out[session_id] = info;  // later rows win
      }
    }
    return true;
  }

}
