#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <zstd.h>
#include <lz4frame.h>

#include "protocol.h"


namespace dicker {
struct StreamCompressor {
  CompressionAlgo algo = CompressionAlgo::None;

  virtual ~StreamCompressor() = default;
  virtual std::size_t update(const std::uint8_t* input, std::size_t inputLen,
                             std::uint8_t* output, std::size_t outputCap) = 0;
  virtual std::size_t flush(std::uint8_t* output, std::size_t outputCap) = 0;
};

struct ZstdCompressor : StreamCompressor {
  ZSTD_CCtx* cctx = nullptr;

  ZstdCompressor() {
    this->algo = CompressionAlgo::Zstd;
    this->cctx = ZSTD_createCCtx();
    if (this->cctx == nullptr) {
      throw std::runtime_error{"failed to create zstd compression context"};
    }
    ZSTD_CCtx_setParameter(this->cctx, ZSTD_c_compressionLevel, 3);
  }

  std::size_t update(const std::uint8_t* input, std::size_t inputLen,
                     std::uint8_t* output, std::size_t outputCap) override {
    ZSTD_inBuffer inBuffer = {input, inputLen, 0};
    ZSTD_outBuffer outBuffer = {output, outputCap, 0};
    while (inBuffer.pos < inBuffer.size) {
      std::size_t result = ZSTD_compressStream2(this->cctx, &outBuffer, &inBuffer, ZSTD_e_continue);
      if (ZSTD_isError(result)) {
        return 0;
      }
      if (outBuffer.pos == outBuffer.size && inBuffer.pos < inBuffer.size) {
        return 0;
      }
    }
    return outBuffer.pos;
  }

  std::size_t flush(std::uint8_t* output, std::size_t outputCap) override {
    ZSTD_inBuffer inBuffer = {nullptr, 0, 0};
    ZSTD_outBuffer outBuffer = {output, outputCap, 0};
    for (;;) {
      std::size_t remaining = ZSTD_compressStream2(this->cctx, &outBuffer, &inBuffer, ZSTD_e_flush);
      if (ZSTD_isError(remaining)) {
        return 0;
      }
      if (remaining == 0) {
        break;
      }
      if (outBuffer.pos == outBuffer.size) {
        return 0;
      }
    }
    return outBuffer.pos;
  }

  ~ZstdCompressor() override {
    ZSTD_freeCCtx(this->cctx);
  }
};

struct Lz4Compressor : StreamCompressor {
  LZ4F_cctx* cctx = nullptr;
  bool started = false;

  Lz4Compressor() {
    this->algo = CompressionAlgo::Lz4;
    LZ4F_errorCode_t code = LZ4F_createCompressionContext(&this->cctx, LZ4F_VERSION);
    if (LZ4F_isError(code)) {
      throw std::runtime_error{"failed to create lz4 compression context"};
    }
  }

  std::size_t update(const std::uint8_t* input, std::size_t inputLen,
                     std::uint8_t* output, std::size_t outputCap) override {
    std::size_t produced = 0;

    if (!this->started) {
      LZ4F_preferences_t preferences;
      std::memset(&preferences, 0, sizeof(preferences));
      preferences.frameInfo.blockMode = LZ4F_blockLinked;
      std::size_t header = LZ4F_compressBegin(this->cctx, output, outputCap, &preferences);
      if (LZ4F_isError(header)) {
        return 0;
      }
      produced += header;
      this->started = true;
    }

    std::size_t body = LZ4F_compressUpdate(this->cctx, output + produced, outputCap - produced,
                                           input, inputLen, nullptr);
    if (LZ4F_isError(body)) {
      return 0;
    }
    produced += body;
    return produced;
  }

  std::size_t flush(std::uint8_t* output, std::size_t outputCap) override {
    std::size_t flushed = LZ4F_flush(this->cctx, output, outputCap, nullptr);
    if (LZ4F_isError(flushed)) {
      return 0;
    }
    return flushed;
  }

  ~Lz4Compressor() override {
    LZ4F_freeCompressionContext(this->cctx);
  }
};

inline std::unique_ptr<StreamCompressor> makeCompressor(CompressionAlgo algo) {
  switch (algo) {
    case CompressionAlgo::Lz4:
      return std::make_unique<Lz4Compressor>();
    case CompressionAlgo::Zstd:
      return std::make_unique<ZstdCompressor>();
    case CompressionAlgo::None:
      return nullptr;
  }
  return nullptr;
}
}  // namespace dicker
