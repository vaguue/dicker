#pragma once

#include <cstdint>
#include <array>
#include <string>
#include "protocol.h"

namespace dicker {

  struct HandshakeRequest {
    std::uint16_t version;
    std::uint8_t flags;
    CompressionAlgo algo;
    std::array<std::uint8_t, kSessionIdSize> session_id;
  };

  HandshakeStatus parse_and_verify_handshake(const std::uint8_t* buffer,
                                             const std::string& key,
                                             HandshakeRequest& out);

  std::string session_id_to_hex(const std::array<std::uint8_t, kSessionIdSize>& session_id);

}
