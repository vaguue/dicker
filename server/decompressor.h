#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include "protocol.h"

namespace dicker {

  struct DecompressSink {
    virtual ~DecompressSink() = default;
    virtual bool write(const std::uint8_t* data, std::size_t len) = 0;
  };

  struct StreamDecompressor {
    virtual ~StreamDecompressor() = default;
    virtual bool decompress(const std::uint8_t* input, std::size_t input_len, DecompressSink& sink) = 0;
  };

  std::unique_ptr<StreamDecompressor> make_decompressor(CompressionAlgo algo);

}
