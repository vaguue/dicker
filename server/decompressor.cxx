#include "decompressor.h"
#include "decompressor_impl.h"

namespace dicker {

  std::unique_ptr<StreamDecompressor> make_decompressor(CompressionAlgo algo) {
    switch (algo) {
      case CompressionAlgo::Lz4:
        return make_lz4_decompressor();
      case CompressionAlgo::Zstd:
        return make_zstd_decompressor();
      case CompressionAlgo::None:
        return nullptr;
    }
    return nullptr;
  }

}
