#pragma once

#include <memory>
#include "decompressor.hpp"

namespace dicker {

  std::unique_ptr<StreamDecompressor> make_lz4_decompressor();
  std::unique_ptr<StreamDecompressor> make_zstd_decompressor();

}
