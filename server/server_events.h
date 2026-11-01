#pragma once

#include <cstdint>
#include <string_view>

#include "muypoco/events.h"

#include "protocol.h"

namespace dicker::server {

  MUYPOCO_BUS(events);

  // "client:connected": an uploader passed the handshake.
  struct ClientConnected {
    std::string_view session;
    std::string_view peer;
    std::string_view compression;
    bool integrity;
    bool tls;
  };

  // "upload:started": a file or a chunk of a big file starts arriving.
  struct UploadStarted {
    std::string_view session;
    std::string_view peer;
    std::string_view path;
    UnitType type;
    std::uint64_t offset;
  };

  // "upload:finished": the unit was written (ok) or abandoned; `reason` says why.
  struct UploadFinished {
    std::string_view session;
    std::string_view peer;
    std::string_view path;
    UnitType type;
    std::uint64_t offset;
    std::uint64_t compressedBytes;
    std::uint64_t bytes;
    bool ok;
    std::string_view reason;
  };

  // "upload:skipped": the server's filter rejected the unit, nothing was sent.
  struct UploadSkipped {
    std::string_view session;
    std::string_view peer;
    std::string_view path;
    UnitType type;
    std::uint64_t offset;
    std::uint64_t length;
  };

  // "client:disconnected": the uploader's connection closed.
  struct ClientDisconnected {
    std::string_view session;
    std::string_view peer;
    std::uint64_t units;
    std::uint64_t bytes;
    std::uint64_t compressedBytes;
  };

}
