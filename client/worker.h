#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <stdexcept>
#include <string_view>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "blake3.h"

#include "protocol.h"
#include "wire.h"
#include "hmac_sha256.h"

#include "net.h"
#include "task.h"
#include "compressor.h"
#include "storageReader.h"
#include "events.h"
#include "log.h"

#include "muypoco/chan.h"


namespace dicker {
using AffinityKey = uint32_t;
constexpr AffinityKey NONE = 0;

const size_t OUT_BUF_SIZE = READ_BLOCK + READ_BLOCK / 16 + 1024;

inline std::string hex(const std::uint8_t* d, std::size_t n) {
  static const char* H = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    s.push_back(H[d[i] >> 4]);
    s.push_back(H[d[i] & 0x0f]);
  }
  return s;
}

inline const char* handshakeStatusName(std::uint8_t s) {
  switch (s) {
    case 0: return "ok";
    case 1: return "bad-magic";
    case 2: return "bad-version";
    case 3: return "auth-failed";
    case 4: return "bad-algo";
    case 5: return "server-error";
    default: return "?";
  }
}

struct PendingUnit {
  uint64_t key;
  Task task;
  bool accepted;
};

struct Worker {
  static constexpr size_t kProbeWindow = 128;

  uint8_t outBuf[OUT_BUF_SIZE];

  bool integrity = false;
  bool acceptMode = false;
  bool hashFirst = false;
  uint64_t nextKey = 0;
  std::vector<PendingUnit> pending;
  muypoco::Chan<Worker, WorkerTask, 128>* chan = nullptr;

  std::atomic<AffinityKey> warm{NONE};
  std::shared_ptr<StorageReaderChan> storage;
  std::unique_ptr<StreamCompressor> compressor;
  Conn conn;
  std::optional<NetworkChan> network;

  Worker(std::unique_ptr<StreamCompressor> compressor, std::shared_ptr<StorageReaderChan> storage, Conn conn)
    : storage{std::move(storage)}, compressor{std::move(compressor)}, conn{std::move(conn)} {
  }

  bool init() noexcept {
    this->integrity = has_flag(this->conn.flags, HandshakeFlag::IntegrityChecks);

    log().info("connecting to %s:%s  algo=%u flags=%u keylen=%zu session=%s",
      this->conn.host.c_str(), this->conn.port.c_str(),
      static_cast<unsigned>(this->compressor->algo),
      static_cast<unsigned>(this->conn.flags),
      this->conn.key.size(),
      hex(this->conn.sessionId.data(), this->conn.sessionId.size()).c_str());

    if (!this->connect()) {
      return false;
    }

    int status = this->shake(kProtocolVersion);

    if (status == static_cast<int>(HandshakeStatus::BadVersion)) {
      log().info("server only speaks protocol %u, reconnecting", static_cast<unsigned>(kMinProtocolVersion));

      if (!this->connect()) {
        return false;
      }
      status = this->shake(kMinProtocolVersion);
    }

    if (status != 0) {
      if (status > 0) {
        log().error("handshake rejected by server: %s", handshakeStatusName(static_cast<std::uint8_t>(status)));
      }
      return false;
    }

    log().info("handshake ok accept=%d hash=%d", this->acceptMode ? 1 : 0, this->hashFirst ? 1 : 0);
    return true;
  }

  bool connect() {
    this->network.reset();
    this->network.emplace(this->conn);
    this->network->start();

    if (!this->network->awaitInit()) {
      log().error("network init failed");
      return false;
    }
    return true;
  }

  int shake(uint16_t version) {
    auto hs = this->handshake(this->conn, version);
    log().info("handshake(%zu)=%s", hs.size(), hex(hs.data(), hs.size()).c_str());

    if (!this->network->await({hs.data(), hs.size()})) {
      log().error("failed to send handshake");
      return -1;
    }

    uint8_t reply[kHandshakeReplySize];

    if (!this->network->worker.recvExact(reply, sizeof(reply))) {
      log().error("no handshake reply (timeout or server closed the connection)");
      return -1;
    }

    const std::uint8_t status = reply[kMagic.size()];

    log().info("reply=%s status=%u (%s)",
               hex(reply, sizeof(reply)).c_str(), status, handshakeStatusName(status));

    if (status != 0) {
      return status;
    }

    uint8_t mode = 0;

    if (version >= 2 && !this->network->worker.recvExact(&mode, sizeof(mode))) {
      log().error("no server mode in the handshake reply");
      return -1;
    }

    this->acceptMode = has_mode(mode, ServerMode::Accept);
    this->hashFirst = has_mode(mode, ServerMode::HashFirst);
    return 0;
  }

  std::vector<uint8_t> handshake(const Conn& conn, uint16_t version) {
    std::vector<uint8_t> out;

    out.insert(out.end(), kMagic.begin(), kMagic.end());

    append_u16(out, version);
    out.push_back(conn.flags);
    out.push_back(static_cast<uint8_t>(this->compressor->algo));
    out.insert(out.end(), conn.sessionId.begin(), conn.sessionId.end());

    uint8_t mac[crypto::kSha256DigestSize];
    crypto::hmac_sha256(reinterpret_cast<const uint8_t*>(conn.key.data()), conn.key.size(),
                        out.data(), out.size(), mac);

    out.insert(out.end(), mac, mac + kHmacSize);
    out.push_back(static_cast<uint8_t>(Cmd::Upload));

    return out;
  }

  bool sendBlock(const uint8_t* data, size_t len) {
    uint8_t header[5];
    header[0] = static_cast<uint8_t>(BlockType::Data);
    write_u32_le(header + 1, static_cast<uint32_t>(len));

    if (!this->network->enqueue({header, sizeof(header)})) {
      return false;
    }
    return this->network->enqueue({data, len});
  }

  void drainStorage() {
    Filled fb;
    while (this->storage->worker.next(fb)) {
      if (fb.eof) {
        return;
      }
      this->storage->worker.recycle(fb.idx);
    }
  }

  bool process(const Task& t) noexcept {
    if (!this->acceptMode) {
      return this->send(t, this->unitHeader(t));
    }

    if (!this->probe(t)) {
      return false;
    }
    if (this->pending.size() >= kProbeWindow || this->chan == nullptr || this->chan->occupancy() == 0) {
      return this->resolve();
    }
    return true;
  }

  std::vector<uint8_t> unitHeader(const Task& t) {
    std::vector<uint8_t> header;
    const std::string_view path = t.pathname;

    header.push_back(static_cast<uint8_t>(t.type));
    append_u16(header, static_cast<uint16_t>(path.size()));
    header.insert(header.end(), path.begin(), path.end());

    if (t.type == UnitType::Chunk) {
      append_u64(header, t.offset);
    }
    return header;
  }

  std::vector<uint8_t> acceptedHeader(uint64_t key) {
    std::vector<uint8_t> header;

    header.push_back(static_cast<uint8_t>(UnitType::Accepted));
    append_u64(header, key);
    return header;
  }

  void hashUnit(const Task& t, uint8_t* out) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    submit(*this->storage, t.type, t.pathname, t.offset, t.length);

    Filled fb;
    while (this->storage->worker.next(fb)) {
      if (fb.eof) {
        break;
      }
      blake3_hasher_update(&hasher, this->storage->worker.data(fb.idx), fb.len);
      this->storage->worker.recycle(fb.idx);
    }

    blake3_hasher_finalize(&hasher, out, kProbeHashSize);
  }

  bool probe(const Task& t) {
    const uint64_t key = this->nextKey++;
    const std::string_view path = t.pathname;
    std::vector<uint8_t> frame;

    frame.push_back(static_cast<uint8_t>(UnitType::Probe));
    append_u64(frame, key);
    frame.push_back(static_cast<uint8_t>(t.type));
    append_u16(frame, static_cast<uint16_t>(path.size()));
    frame.insert(frame.end(), path.begin(), path.end());
    append_u64(frame, t.offset);
    append_u64(frame, t.length);
    append_u64(frame, t.fileSize);
    append_u64(frame, static_cast<uint64_t>(t.mtime));

    if (this->hashFirst) {
      uint8_t hash[kProbeHashSize];
      this->hashUnit(t, hash);
      frame.insert(frame.end(), hash, hash + kProbeHashSize);
    }

    this->pending.push_back(PendingUnit{key, t, false});
    return this->network->enqueue({frame.data(), frame.size()});
  }

  bool resolve() {
    const bool drained = this->network->await({nullptr, 0});
    bool ok = drained;

    for (PendingUnit& unit : this->pending) {
      uint8_t reply[kProbeReplySize];

      if (!ok || !this->network->worker.recvExact(reply, sizeof(reply)) || read_u64_le(reply + 1) != unit.key) {
        ok = false;
        break;
      }
      unit.accepted = reply[0] == static_cast<uint8_t>(ProbeDecision::Accept);
    }

    for (const PendingUnit& unit : this->pending) {
      if (!ok) {
        break;
      }
      if (unit.accepted) {
        ok = this->send(unit.task, this->acceptedHeader(unit.key));
      }
      else {
        events.emit<"upload:skipped">(UploadSkipped{unit.task.pathname, unit.task.type, unit.task.offset, unit.task.length, unit.task.fileSize});
      }
    }

    this->pending.clear();
    return ok;
  }

  bool send(const Task& t, const std::vector<uint8_t>& header) noexcept {
    const std::string_view path = t.pathname;

    events.emit<"upload:started">(UploadStarted{path, t.type, t.offset, t.length, t.fileSize});

    uint64_t compressedBytes = 0;
    const bool ok = this->upload(t, header, compressedBytes);

    events.emit<"upload:finished">(UploadFinished{path, t.type, t.offset, t.length, t.fileSize, compressedBytes, ok});
    return ok;
  }

  bool upload(const Task& t, const std::vector<uint8_t>& header, uint64_t& compressedBytes) noexcept {
    if (!this->network->enqueue({header.data(), header.size()})) {
      return false;
    }

    XXH64_state_t checksum;
    if (this->integrity) {
      XXH64_reset(&checksum, 0);
    }

    submit(*this->storage, t.type, t.pathname, t.offset, t.length);

    Filled fb;
    while (this->storage->worker.next(fb)) {
      if (fb.eof) {
        break;
      }

      uint8_t* data = this->storage->worker.data(fb.idx);

      if (this->integrity) {
        XXH64_update(&checksum, data, fb.len);
      }

      size_t produced = this->compressor->update(data, fb.len, this->outBuf, OUT_BUF_SIZE);
      if (produced > 0) {
        if (!this->sendBlock(this->outBuf, produced)) {
          this->storage->worker.recycle(fb.idx);
          this->drainStorage();
          return false;
        }
        compressedBytes += produced;
      }

      this->storage->worker.recycle(fb.idx);
    }

    size_t flushed = this->compressor->flush(this->outBuf, OUT_BUF_SIZE);
    if (flushed > 0) {
      if (!this->sendBlock(this->outBuf, flushed)) {
        return false;
      }
      compressedBytes += flushed;
    }

    uint8_t endOfUnit = static_cast<uint8_t>(BlockType::EndOfUnit);
    if (!this->network->enqueue({&endOfUnit, 1})) {
      return false;
    }

    if (this->integrity) {
      uint8_t checksumBytes[8];
      write_u64_le(checksumBytes, XXH64_digest(&checksum));
      if (!this->network->enqueue({checksumBytes, sizeof(checksumBytes)})) {
        return false;
      }
    }

    return true;
  }

  void finish() {
    auto eof = static_cast<uint8_t>(UnitType::End);
    this->network->enqueue({&eof, 1});
  }
};

using WorkerChan = muypoco::Chan<Worker, WorkerTask, 128>;
}  // namespace dicker
