#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <cstring>
#include "sha256.hpp"

namespace dicker::crypto {

  inline void hmac_sha256(const std::uint8_t* key, std::size_t key_len,
                          const std::uint8_t* message, std::size_t message_len,
                          std::uint8_t out[kSha256DigestSize]) {
    std::array<std::uint8_t, kSha256BlockSize> block_key;
    block_key.fill(0);

    if (key_len > kSha256BlockSize) {
      sha256(key, key_len, block_key.data());
    }
    else {
      std::memcpy(block_key.data(), key, key_len);
    }

    std::array<std::uint8_t, kSha256BlockSize> inner_pad;
    std::array<std::uint8_t, kSha256BlockSize> outer_pad;
    for (std::size_t i = 0; i < kSha256BlockSize; ++i) {
      inner_pad[i] = block_key[i] ^ 0x36;
      outer_pad[i] = block_key[i] ^ 0x5c;
    }

    std::array<std::uint8_t, kSha256DigestSize> inner_digest;
    Sha256 inner;
    inner.update(inner_pad.data(), inner_pad.size());
    inner.update(message, message_len);
    inner.finish(inner_digest.data());

    Sha256 outer;
    outer.update(outer_pad.data(), outer_pad.size());
    outer.update(inner_digest.data(), inner_digest.size());
    outer.finish(out);
  }

  inline bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t len) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < len; ++i) {
      difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return difference == 0;
  }

}
