#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace dicker {

  inline std::uint16_t read_u16_le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
  }

  inline std::uint32_t read_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
  }

  inline std::uint64_t read_u64_le(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(p[0]) |
           (static_cast<std::uint64_t>(p[1]) << 8) |
           (static_cast<std::uint64_t>(p[2]) << 16) |
           (static_cast<std::uint64_t>(p[3]) << 24) |
           (static_cast<std::uint64_t>(p[4]) << 32) |
           (static_cast<std::uint64_t>(p[5]) << 40) |
           (static_cast<std::uint64_t>(p[6]) << 48) |
           (static_cast<std::uint64_t>(p[7]) << 56);
  }

  inline void write_u16_le(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
  }

  inline void write_u32_le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
  }

  inline void write_u64_le(std::uint8_t* p, std::uint64_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
    p[4] = static_cast<std::uint8_t>(v >> 32);
    p[5] = static_cast<std::uint8_t>(v >> 40);
    p[6] = static_cast<std::uint8_t>(v >> 48);
    p[7] = static_cast<std::uint8_t>(v >> 56);
  }

  inline void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    std::uint8_t buffer[2];
    write_u16_le(buffer, value);
    out.insert(out.end(), buffer, buffer + 2);
  }

  inline void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    std::uint8_t buffer[8];
    write_u64_le(buffer, value);
    out.insert(out.end(), buffer, buffer + 8);
  }

}
