#include "decompressor_impl.hpp"

#include <vector>
#include <lz4frame.h>

namespace dicker {

  namespace {

    constexpr std::size_t kDecodeChunk = 256 * 1024;

    struct Lz4Decompressor : StreamDecompressor {
      LZ4F_dctx* context_ = nullptr;
      bool valid_ = false;
      std::vector<std::uint8_t> output_;

      Lz4Decompressor() : output_(kDecodeChunk) {
        LZ4F_errorCode_t code = LZ4F_createDecompressionContext(&context_, LZ4F_VERSION);
        valid_ = !LZ4F_isError(code);
      }

      ~Lz4Decompressor() override {
        if (context_ != nullptr) {
          LZ4F_freeDecompressionContext(context_);
        }
      }

      Lz4Decompressor(const Lz4Decompressor&) = delete;
      Lz4Decompressor& operator=(const Lz4Decompressor&) = delete;

      bool valid() const {
        return valid_;
      }

      bool decompress(const std::uint8_t* input, std::size_t input_len, DecompressSink& sink) override {
        std::size_t input_pos = 0;
        while (input_pos < input_len) {
          std::size_t destination_size = output_.size();
          std::size_t source_size = input_len - input_pos;
          std::size_t hint = LZ4F_decompress(context_,
                                             output_.data(), &destination_size,
                                             input + input_pos, &source_size,
                                             nullptr);
          if (LZ4F_isError(hint)) {
            return false;
          }
          input_pos += source_size;
          if (destination_size > 0) {
            if (!sink.write(output_.data(), destination_size)) {
              return false;
            }
          }
          if (source_size == 0 && destination_size == 0) {
            break;
          }
        }
        return input_pos == input_len;
      }
    };

  }

  std::unique_ptr<StreamDecompressor> make_lz4_decompressor() {
    auto decompressor = std::make_unique<Lz4Decompressor>();
    if (!decompressor->valid()) {
      return nullptr;
    }
    return decompressor;
  }

}
