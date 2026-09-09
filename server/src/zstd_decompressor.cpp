#include "decompressor_impl.hpp"

#include <vector>
#include <zstd.h>

namespace dicker {

  namespace {

    constexpr int kWindowLogMax = 31;
    constexpr std::size_t kDecodeChunk = 256 * 1024;

    struct ZstdDecompressor : StreamDecompressor {
      ZSTD_DCtx* context_ = nullptr;
      std::vector<std::uint8_t> output_;

      ZstdDecompressor() : output_(kDecodeChunk) {
        context_ = ZSTD_createDCtx();
        if (context_ != nullptr) {
          ZSTD_DCtx_setParameter(context_, ZSTD_d_windowLogMax, kWindowLogMax);
        }
      }

      ~ZstdDecompressor() override {
        if (context_ != nullptr) {
          ZSTD_freeDCtx(context_);
        }
      }

      ZstdDecompressor(const ZstdDecompressor&) = delete;
      ZstdDecompressor& operator=(const ZstdDecompressor&) = delete;

      bool valid() const {
        return context_ != nullptr;
      }

      bool decompress(const std::uint8_t* input, std::size_t input_len, DecompressSink& sink) override {
        ZSTD_inBuffer in_buffer = {input, input_len, 0};
        while (in_buffer.pos < in_buffer.size) {
          ZSTD_outBuffer out_buffer = {output_.data(), output_.size(), 0};
          std::size_t result = ZSTD_decompressStream(context_, &out_buffer, &in_buffer);
          if (ZSTD_isError(result)) {
            return false;
          }
          if (out_buffer.pos > 0) {
            if (!sink.write(output_.data(), out_buffer.pos)) {
              return false;
            }
          }
          if (out_buffer.pos == 0 && in_buffer.pos < in_buffer.size) {
            return false;
          }
        }
        return true;
      }
    };

  }

  std::unique_ptr<StreamDecompressor> make_zstd_decompressor() {
    auto decompressor = std::make_unique<ZstdDecompressor>();
    if (!decompressor->valid()) {
      return nullptr;
    }
    return decompressor;
  }

}
