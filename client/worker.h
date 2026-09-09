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
#include "conn.h"
#include "task.h"
#include "compressor.h"
#include "storageReader.h"
#include "queue.h"

using namespace dicker;

const size_t MAX_BUF_SIZE = 256 * 1024;
const size_t OUT_BUF_SIZE = MAX_BUF_SIZE + MAX_BUF_SIZE / 16 + 1024;

struct Worker : Queue<Task, Worker> {
  uint8_t inBuf[MAX_BUF_SIZE];
  uint8_t outBuf[OUT_BUF_SIZE];

  net::socket_t fd = net::INVALID;
  bool integrity = false;
  std::shared_ptr<StorageReader> storage;
  std::unique_ptr<StreamCompressor> compressor;

  Worker(std::unique_ptr<StreamCompressor> compressor, std::shared_ptr<StorageReader> storage)
    : storage{std::move(storage)}, compressor{std::move(compressor)} {
  }

  void start(const Conn& conn) {
    this->integrity = has_flag(conn.flags, HandshakeFlag::IntegrityChecks);
    this->fd = net::tcpConnect(conn.host, conn.port, conn.proxy);

    if (!net::isValid(this->fd)) {
      throw std::runtime_error{"failed to connect to server"};
    }

    auto hs = this->handshake(conn);

    if (!net::sendAll(this->fd, hs.data(), hs.size())) {
      throw std::runtime_error{"failed to send handshake"};
    }

    uint8_t reply[kHandshakeReplySize];
    if (!net::recvExact(this->fd, reply, sizeof(reply)) || reply[kMagic.size()] != 0) {
      throw std::runtime_error{"handshake rejected by server"};
    }

    this->startQueue();
  }

  std::vector<uint8_t> handshake(const Conn& conn) {
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
    net::sendAll(this->fd, header, sizeof(header));
    net::sendAll(this->fd, data, len);
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
    net::sendAll(this->fd, header.data(), header.size());

    XXH64_state_t checksum;
    if (this->integrity) {
      XXH64_reset(&checksum, 0);
    }

    size_t remaining = t.length;
    size_t fileOffset = t.offset;
    while (remaining > 0) {
      size_t want = std::min(remaining, MAX_BUF_SIZE);
      size_t got = this->storage->read_range(this->inBuf, t.pathname, fileOffset, want);
      if (got == 0) {
        break;
      }
      if (this->integrity) {
        XXH64_update(&checksum, this->inBuf, got);
      }
      size_t produced = this->compressor->update(this->inBuf, got, this->outBuf, OUT_BUF_SIZE);
      if (produced > 0) {
        this->sendBlock(this->outBuf, produced);
      }
      remaining -= got;
      fileOffset += got;
    }

    size_t flushed = this->compressor->flush(this->outBuf, OUT_BUF_SIZE);
    if (flushed > 0) {
      this->sendBlock(this->outBuf, flushed);
    }

    uint8_t endOfUnit = static_cast<uint8_t>(BlockType::EndOfUnit);
    net::sendAll(this->fd, &endOfUnit, 1);

    if (this->integrity) {
      uint8_t checksumBytes[8];
      write_u64_le(checksumBytes, XXH64_digest(&checksum));
      net::sendAll(this->fd, checksumBytes, sizeof(checksumBytes));
    }
  }

  void finish() {
    auto eof = static_cast<uint8_t>(UnitType::End);
    net::sendAll(this->fd, &eof, 1);
  }
};
