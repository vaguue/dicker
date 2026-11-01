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

#include "json.hpp"

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

  }  // namespace

  // ---------------- TokenStore ----------------

  namespace {

    std::string tokens_to_json(const std::vector<std::string>& tokens) {
      nlohmann::json doc;
      doc["tokens"] = tokens;
      return doc.dump();
    }

    bool tokens_from_json(const std::string& text, std::vector<std::string>& out,
                          std::string& err) {
      out.clear();
      nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
      if (doc.is_discarded() || !doc.is_object()) {
        err = "malformed token json";
        return false;
      }
      auto it = doc.find("tokens");
      if (it == doc.end()) {
        return true;  // no tokens key -> empty store
      }
      if (!it->is_array()) {
        err = "\"tokens\" is not an array";
        return false;
      }
      for (const auto& token : *it) {
        if (!token.is_string()) {
          err = "token is not a string";
          return false;
        }
        out.push_back(token.get<std::string>());
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
      nlohmann::ordered_json row;
      row["sessionId"] = session_id;
      row["ip"] = ip;
      row["created"] = created;
      return row.dump();
    }

    bool session_row_from_json(const std::string& text, std::string& session_id,
                               SessionInfo& info) {
      nlohmann::json row = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
      if (row.is_discarded() || !row.is_object()) {
        return false;
      }
      auto id = row.find("sessionId");
      if (id == row.end() || !id->is_string()) {
        return false;  // need at least sessionId
      }
      session_id = id->get<std::string>();

      auto ip = row.find("ip");
      if (ip != row.end() && ip->is_string()) {
        info.ip = ip->get<std::string>();
      }
      auto created = row.find("created");
      if (created != row.end() && created->is_number_unsigned()) {
        info.created = created->get<std::uint64_t>();
      }
      return true;
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
