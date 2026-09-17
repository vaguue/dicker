#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace dicker {

  // Bearer tokens for the HTTP endpoints, stored as an encrypted whole-file JSON
  // object {"tokens":[...]}. Sealed with AES-256-GCM under a key derived from the
  // shared secret (SHA-256(DICKER_KEY)); on-disk layout is
  // [12-byte nonce][16-byte tag][ciphertext]. Rewritten atomically (temp file +
  // fsync + rename); the only writer is dicker-dbtool.
  struct TokenStore {
    std::vector<std::string> tokens;

    // A missing file yields an empty store and returns true; a present but
    // undecryptable/corrupt file returns false so the caller can refuse to
    // overwrite it.
    static bool load(const std::string& path, const std::string& key,
                     TokenStore& out, std::string& err);
    bool save(const std::string& path, const std::string& key, std::string& err) const;

    bool verify(const std::string& token) const;  // constant-time membership
  };

  struct SessionInfo {
    std::string ip;              // uploader IP at handshake time
    std::uint64_t created = 0;   // unix seconds
  };

  // Append-only log of upload sessions. Each line is an independently sealed row
  // hex(nonce||tag||ciphertext) over a small JSON object, so appends stay atomic
  // (a single O_APPEND write) and a torn/corrupt final line is simply skipped on
  // read. The only writer is the server's libuv loop thread.
  struct SessionLog {
    static bool append(const std::string& path, const std::string& key,
                       const std::string& session_id, const std::string& ip,
                       std::uint64_t created, std::string& err);

    // Read every decodable row into `out` (later rows win per session id).
    // Best effort: undecryptable/unparseable rows are skipped. Returns false only
    // if the file exists but cannot be opened.
    static bool load_all(const std::string& path, const std::string& key,
                         std::map<std::string, SessionInfo>& out);
  };

}
