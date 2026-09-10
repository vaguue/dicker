#pragma once

#include <cstdint>
#include <cstddef>
#include <array>

namespace dicker {

  constexpr std::array<std::uint8_t, 4> kMagic = {'D', 'K', 'R', '1'};
  constexpr std::uint16_t kProtocolVersion = 1;

  enum struct CompressionAlgo : std::uint8_t {
    None = 0,
    Lz4 = 1,
    Zstd = 2,
  };

  enum struct UnitType : std::uint8_t {
    File = 0,
    Chunk = 1,
    End = 0xFF,
  };

  enum struct BlockType : std::uint8_t {
    EndOfUnit = 0,
    Data = 1,
  };

  enum struct HandshakeFlag : std::uint8_t {
    IntegrityChecks = 1 << 0,
  };

  enum struct HandshakeStatus : std::uint8_t {
    Ok = 0,
    BadMagic = 1,
    BadVersion = 2,
    AuthFailed = 3,
    BadAlgo = 4,
    ServerError = 5,
  };

  constexpr std::size_t kSessionIdSize = 16;
  constexpr std::size_t kHmacSize = 32;

  constexpr std::size_t kHandshakeSignedPrefixSize =
    kMagic.size() + sizeof(std::uint16_t) + 1 + 1 + kSessionIdSize;

  constexpr std::size_t kHandshakeRequestSize =
    kHandshakeSignedPrefixSize + kHmacSize;

  constexpr std::size_t kHandshakeReplySize = kMagic.size() + 1;

  constexpr std::uint16_t kMaxPathLength = 4096;

  constexpr std::uint32_t kMaxBlockSize = 64u * 1024 * 1024;

  inline bool has_flag(std::uint8_t flags, HandshakeFlag flag) {
    return (flags & static_cast<std::uint8_t>(flag)) != 0;
  }

}
