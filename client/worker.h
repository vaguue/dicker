#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <stdexcept>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "protocol.hpp"
#include "wire.hpp"
#include "hmac_sha256.hpp"

#include "net.h"
#include "task.h"
#include "compressor.h"
#include "storageReader.h"
#include "chan.h"

using namespace dicker;

const size_t OUT_BUF_SIZE = READ_BLOCK + READ_BLOCK / 16 + 1024;

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

  // Runs on the worker's own consumer thread (invoked by Chan::run), so the
  // handshake and every later block share one producer into `network`.
  void init() {
    this->integrity = has_flag(this->conn.flags, HandshakeFlag::IntegrityChecks);

    this->network.init(this->conn);
    this->network.start();

    auto hs = this->handshake(this->conn);

    if (!this->network.await({hs.data(), hs.size()})) {
      throw std::runtime_error{"failed to send handshake"};
    }

    uint8_t reply[kHandshakeReplySize];
    if (!this->network.recvExact(reply, sizeof(reply)) || reply[kMagic.size()] != 0) {
      throw std::runtime_error{"handshake rejected by server"};
    }
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
