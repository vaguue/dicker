#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <stdexcept>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "protocol.h"
#include "wire.h"
#include "hmac_sha256.h"

#include "net.h"
#include "task.h"
#include "compressor.h"
#include "storageReader.h"
#include "chan.h"


namespace dicker {
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

struct Worker : Chan<Worker, WorkerTask, 128> {
  uint8_t outBuf[OUT_BUF_SIZE];

  bool integrity = false;

  std::shared_ptr<StorageReader> storage;
  std::unique_ptr<StreamCompressor> compressor;
  NetworkClient network;
  Conn conn;

  Worker(std::unique_ptr<StreamCompressor> compressor, std::shared_ptr<StorageReader> storage, Conn conn)
    : storage{std::move(storage)}, compressor{std::move(compressor)}, conn{std::move(conn)} {
  }

  // The consumer thread produces into `network` and drives `storage`, so it must
  // be joined before those members are destroyed. The base ~Chan would join it
  // after members are gone (base is destroyed last), so join it here first. It
  // drains any queued units before returning.
  ~Worker() {
    this->stopped.store(true, std::memory_order_release);
    this->notEmptyGate.fetch_add(1, std::memory_order_release);
    this->notEmptyGate.notify_all();
    this->notFullGate.fetch_add(1, std::memory_order_release);
    this->notFullGate.notify_all();
    if (this->th.joinable()) {
      this->th.join();
    }
  }

  // Runs on the worker's own consumer thread (invoked by Chan::run), so the
  // handshake and every later block share one producer into `network`.
  void init() {
    this->integrity = has_flag(this->conn.flags, HandshakeFlag::IntegrityChecks);

    std::fprintf(stderr,
      "[dicker] connecting to %s:%s  algo=%u flags=%u keylen=%zu session=%s\n",
      this->conn.host.c_str(), this->conn.port.c_str(),
      static_cast<unsigned>(this->compressor->algo),
      static_cast<unsigned>(this->conn.flags),
      this->conn.key.size(),
      hex(this->conn.sessionId.data(), this->conn.sessionId.size()).c_str());

    this->network.init(this->conn);
    this->network.start();

    auto hs = this->handshake(this->conn);
    std::fprintf(stderr, "[dicker] handshake(%zu)=%s\n",
                 hs.size(), hex(hs.data(), hs.size()).c_str());

    if (!this->network.await({hs.data(), hs.size()})) {
      throw std::runtime_error{"failed to send handshake"};
    }

    uint8_t reply[kHandshakeReplySize];
    if (!this->network.recvExact(reply, sizeof(reply))) {
      throw std::runtime_error{"no handshake reply (server closed the connection)"};
    }

    const std::uint8_t status = reply[kMagic.size()];
    std::fprintf(stderr, "[dicker] reply=%s status=%u (%s)\n",
                 hex(reply, sizeof(reply)).c_str(), status, handshakeStatusName(status));
    if (status != 0) {
      throw std::runtime_error{std::string("handshake rejected by server: ") +
                               handshakeStatusName(status)};
    }
    std::fprintf(stderr, "[dicker] handshake ok\n");
  }

  std::vector<uint8_t> handshake(const Conn& conn) { //TODO move to Conn I think
    std::vector<uint8_t> out;

    out.insert(out.end(), kMagic.begin(), kMagic.end());

    append_u16(out, kProtocolVersion);
    out.push_back(conn.flags);
    out.push_back(static_cast<uint8_t>(this->compressor->algo));
    out.insert(out.end(), conn.sessionId.begin(), conn.sessionId.end());

    uint8_t mac[crypto::kSha256DigestSize];
    crypto::hmac_sha256(reinterpret_cast<const uint8_t*>(conn.key.data()), conn.key.size(),
                        out.data(), out.size(), mac);

    out.insert(out.end(), mac, mac + kHmacSize);

    return out;
  }

  void sendBlock(const uint8_t* data, size_t len) {
    uint8_t header[5];
    header[0] = static_cast<uint8_t>(BlockType::Data);
    write_u32_le(header + 1, static_cast<uint32_t>(len));

    this->network.enqueue({header, sizeof(header)});
    this->network.enqueue({data, len});
  }

  void process(const Task& t) {
    std::vector<uint8_t> header;

    header.push_back(static_cast<uint8_t>(t.type));

    std::string path = t.pathname;

    append_u16(header, static_cast<uint16_t>(path.size()));
    header.insert(header.end(), path.begin(), path.end());

    if (t.type == UnitType::Chunk) {
      append_u64(header, t.offset);
    }
    this->network.enqueue({header.data(), header.size()});

    XXH64_state_t checksum;
    if (this->integrity) {
      XXH64_reset(&checksum, 0);
    }

    this->storage->submit(t.type, t.pathname, t.offset, t.length);

    Filled fb;
    while (this->storage->next(fb)) {
      if (fb.eof) {
        break;
      }

      uint8_t* data = this->storage->data(fb.idx);

      if (this->integrity) {
        XXH64_update(&checksum, data, fb.len);
      }

      size_t produced = this->compressor->update(data, fb.len, this->outBuf, OUT_BUF_SIZE);
      if (produced > 0) {
        this->sendBlock(this->outBuf, produced);
      }

      this->storage->recycle(fb.idx);
    }

    size_t flushed = this->compressor->flush(this->outBuf, OUT_BUF_SIZE);
    if (flushed > 0) {
      this->sendBlock(this->outBuf, flushed);
    }

    uint8_t endOfUnit = static_cast<uint8_t>(BlockType::EndOfUnit);
    this->network.enqueue({&endOfUnit, 1});

    if (this->integrity) {
      uint8_t checksumBytes[8];
      write_u64_le(checksumBytes, XXH64_digest(&checksum));
      this->network.enqueue({checksumBytes, sizeof(checksumBytes)});
    }
  }

  void finish() {
    auto eof = static_cast<uint8_t>(UnitType::End);
    this->network.enqueue({&eof, 1});
  }
};
}  // namespace dicker
