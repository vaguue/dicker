#include "handshake.hpp"

#include <cstring>
#include "wire.hpp"
#include "hmac_sha256.hpp"

namespace dicker {

  namespace {

    bool magic_matches(const std::uint8_t* buffer) {
      return std::memcmp(buffer, kMagic.data(), kMagic.size()) == 0;
    }

    bool algo_supported(CompressionAlgo algo) {
      return algo == CompressionAlgo::Lz4 || algo == CompressionAlgo::Zstd;
    }

  }

  HandshakeStatus parse_and_verify_handshake(const std::uint8_t* buffer,
                                             const std::string& key,
                                             HandshakeRequest& out) {
    if (!magic_matches(buffer)) {
      return HandshakeStatus::BadMagic;
    }

    std::uint8_t expected_hmac[crypto::kSha256DigestSize];
    crypto::hmac_sha256(reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
                        buffer, kHandshakeSignedPrefixSize, expected_hmac);

    const std::uint8_t* provided_hmac = buffer + kHandshakeSignedPrefixSize;
    if (!crypto::constant_time_equal(expected_hmac, provided_hmac, kHmacSize)) {
      return HandshakeStatus::AuthFailed;
    }

    std::uint16_t version = read_u16_le(buffer + kMagic.size());
    if (version != kProtocolVersion) {
      return HandshakeStatus::BadVersion;
    }

    out.version = version;
    out.flags = buffer[kMagic.size() + sizeof(std::uint16_t)];
    out.algo = static_cast<CompressionAlgo>(buffer[kMagic.size() + sizeof(std::uint16_t) + 1]);
    std::memcpy(out.session_id.data(),
                buffer + kMagic.size() + sizeof(std::uint16_t) + 2,
                kSessionIdSize);

    if (!algo_supported(out.algo)) {
      return HandshakeStatus::BadAlgo;
    }

    return HandshakeStatus::Ok;
  }

  std::string session_id_to_hex(const std::array<std::uint8_t, kSessionIdSize>& session_id) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(kSessionIdSize * 2);
    for (std::uint8_t byte : session_id) {
      result.push_back(digits[byte >> 4]);
      result.push_back(digits[byte & 0x0f]);
    }
    return result;
  }

}
