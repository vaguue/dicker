#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <cstring>

namespace dicker::crypto {

  constexpr std::size_t kSha256DigestSize = 32;
  constexpr std::size_t kSha256BlockSize = 64;

  namespace detail {

    inline constexpr std::array<std::uint32_t, 64> kRoundConstants = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    inline std::uint32_t rotate_right(std::uint32_t value, std::uint32_t bits) {
      return (value >> bits) | (value << (32 - bits));
    }

  }

  struct Sha256 {
    Sha256() {
      reset();
    }

    void reset() {
      state_ = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
      };
      bit_count_ = 0;
      buffer_len_ = 0;
    }

    void update(const std::uint8_t* data, std::size_t len) {
      bit_count_ += static_cast<std::uint64_t>(len) * 8;
      while (len > 0) {
        std::size_t space = kSha256BlockSize - buffer_len_;
        std::size_t take = len < space ? len : space;
        std::memcpy(buffer_.data() + buffer_len_, data, take);
        buffer_len_ += take;
        data += take;
        len -= take;
        if (buffer_len_ == kSha256BlockSize) {
          process_block(buffer_.data());
          buffer_len_ = 0;
        }
      }
    }

    void finish(std::uint8_t out[kSha256DigestSize]) {
      std::uint64_t total_bits = bit_count_;
      std::uint8_t one = 0x80;
      update(&one, 1);

      std::uint8_t zero = 0x00;
      while (buffer_len_ != kSha256BlockSize - 8) {
        update(&zero, 1);
      }

      std::uint8_t length_block[8];
      for (std::size_t i = 0; i < 8; ++i) {
        length_block[i] = static_cast<std::uint8_t>(total_bits >> (56 - i * 8));
      }
      update(length_block, 8);

      for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
      }
    }

    void process_block(const std::uint8_t* block) {
      std::array<std::uint32_t, 64> w;
      for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]));
      }
      for (std::size_t i = 16; i < 64; ++i) {
        std::uint32_t s0 = detail::rotate_right(w[i - 15], 7) ^ detail::rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = detail::rotate_right(w[i - 2], 17) ^ detail::rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }

      std::uint32_t a = state_[0];
      std::uint32_t b = state_[1];
      std::uint32_t c = state_[2];
      std::uint32_t d = state_[3];
      std::uint32_t e = state_[4];
      std::uint32_t f = state_[5];
      std::uint32_t g = state_[6];
      std::uint32_t h = state_[7];

      for (std::size_t i = 0; i < 64; ++i) {
        std::uint32_t big_s1 = detail::rotate_right(e, 6) ^ detail::rotate_right(e, 11) ^ detail::rotate_right(e, 25);
        std::uint32_t choose = (e & f) ^ (~e & g);
        std::uint32_t temp1 = h + big_s1 + choose + detail::kRoundConstants[i] + w[i];
        std::uint32_t big_s0 = detail::rotate_right(a, 2) ^ detail::rotate_right(a, 13) ^ detail::rotate_right(a, 22);
        std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t temp2 = big_s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
      }

      state_[0] += a;
      state_[1] += b;
      state_[2] += c;
      state_[3] += d;
      state_[4] += e;
      state_[5] += f;
      state_[6] += g;
      state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_;
    std::uint64_t bit_count_;
    std::array<std::uint8_t, kSha256BlockSize> buffer_;
    std::size_t buffer_len_;
  };

  inline void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out[kSha256DigestSize]) {
    Sha256 hasher;
    hasher.update(data, len);
    hasher.finish(out);
  }

}
