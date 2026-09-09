#pragma once

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace affinity {

enum class Flavor : uint8_t {
  none,
  opaque,
  prose,
  structured,
  tabular,
  code,
  logs,
  generic
};

enum class AffinityKey : uint32_t {};

struct AffinityVector {
  uint8_t structure{0};
  uint8_t tokens{0};
  uint8_t records{0};
  bool opaque{false};
  bool cold{false};
};

constexpr uint32_t kOpaqueBit = 1u << 24;
constexpr uint32_t kColdBit = 1u << 25;
constexpr uint32_t kColdCost = 16;
constexpr uint32_t kWallCost = 1u << 20;

constexpr AffinityKey packAffinity(AffinityVector vector) {
  uint32_t value = uint32_t(vector.structure)
    | (uint32_t(vector.tokens) << 8)
    | (uint32_t(vector.records) << 16);
  if (vector.opaque) {
    value |= kOpaqueBit;
  }
  if (vector.cold) {
    value |= kColdBit;
  }
  return AffinityKey(value);
}

constexpr AffinityVector unpackAffinity(AffinityKey key) {
  const uint32_t value = uint32_t(key);
  AffinityVector vector{};
  vector.structure = uint8_t(value & 0xFF);
  vector.tokens = uint8_t((value >> 8) & 0xFF);
  vector.records = uint8_t((value >> 16) & 0xFF);
  vector.opaque = (value & kOpaqueBit) != 0;
  vector.cold = (value & kColdBit) != 0;
  return vector;
}

constexpr AffinityKey affinityOf(Flavor flavor) {
  switch (flavor) {
    case Flavor::none: {
      return packAffinity(AffinityVector{0, 0, 0, false, true});
    }
    case Flavor::opaque: {
      return packAffinity(AffinityVector{0, 0, 0, true, false});
    }
    case Flavor::prose: {
      return packAffinity(AffinityVector{0, 0, 0, false, false});
    }
    case Flavor::structured: {
      return packAffinity(AffinityVector{9, 0, 0, false, false});
    }
    case Flavor::tabular: {
      return packAffinity(AffinityVector{3, 0, 9, false, false});
    }
    case Flavor::code: {
      return packAffinity(AffinityVector{2, 9, 2, false, false});
    }
    case Flavor::logs: {
      return packAffinity(AffinityVector{2, 0, 8, false, false});
    }
    case Flavor::generic: {
      return packAffinity(AffinityVector{1, 0, 1, false, false});
    }
  }
  return packAffinity(AffinityVector{0, 0, 0, false, true});
}

inline constexpr AffinityKey affinityNone = affinityOf(Flavor::none);

inline uint32_t distance(AffinityKey incoming, AffinityKey resident) {
  const AffinityVector left = unpackAffinity(incoming);
  const AffinityVector right = unpackAffinity(resident);
  if (right.cold) {
    return kColdCost;
  }
  if (left.opaque != right.opaque) {
    return kWallCost;
  }
  if (left.opaque && right.opaque) {
    return 0;
  }
  const int32_t deltaStructure = int32_t(left.structure) - int32_t(right.structure);
  const int32_t deltaTokens = int32_t(left.tokens) - int32_t(right.tokens);
  const int32_t deltaRecords = int32_t(left.records) - int32_t(right.records);
  return uint32_t(deltaStructure * deltaStructure
    + deltaTokens * deltaTokens
    + deltaRecords * deltaRecords);
}

inline const std::unordered_map<std::string, Flavor>& flavorTable() {
  static const std::unordered_map<std::string, Flavor> table = {
    {"png", Flavor::opaque}, {"jpg", Flavor::opaque}, {"jpeg", Flavor::opaque},
    {"gif", Flavor::opaque}, {"webp", Flavor::opaque}, {"mp4", Flavor::opaque},
    {"mkv", Flavor::opaque}, {"mov", Flavor::opaque}, {"webm", Flavor::opaque},
    {"mp3", Flavor::opaque}, {"flac", Flavor::opaque}, {"ogg", Flavor::opaque},
    {"zip", Flavor::opaque}, {"gz", Flavor::opaque}, {"zst", Flavor::opaque},
    {"xz", Flavor::opaque}, {"7z", Flavor::opaque}, {"rar", Flavor::opaque},
    {"pdf", Flavor::opaque}, {"docx", Flavor::opaque}, {"xlsx", Flavor::opaque},
    {"exe", Flavor::opaque}, {"dll", Flavor::opaque}, {"woff2", Flavor::opaque},
    {"txt", Flavor::prose}, {"md", Flavor::prose}, {"rst", Flavor::prose},
    {"json", Flavor::structured}, {"xml", Flavor::structured}, {"html", Flavor::structured},
    {"htm", Flavor::structured}, {"yaml", Flavor::structured}, {"yml", Flavor::structured},
    {"toml", Flavor::structured}, {"svg", Flavor::structured},
    {"csv", Flavor::tabular}, {"tsv", Flavor::tabular},
    {"c", Flavor::code}, {"h", Flavor::code}, {"cpp", Flavor::code},
    {"hpp", Flavor::code}, {"cc", Flavor::code}, {"py", Flavor::code},
    {"js", Flavor::code}, {"ts", Flavor::code}, {"go", Flavor::code},
    {"rs", Flavor::code}, {"java", Flavor::code}, {"sql", Flavor::code},
    {"log", Flavor::logs}
  };
  return table;
}

inline std::string loweredExtension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  if (!extension.empty() && extension.front() == '.') {
    extension.erase(extension.begin());
  }
  for (char& character : extension) {
    character = char(std::tolower(uint8_t(character)));
  }
  return extension;
}

inline Flavor classify(const std::filesystem::path& path) {
  const std::string extension = loweredExtension(path);
  if (extension.empty()) {
    return Flavor::generic;
  }
  const auto found = flavorTable().find(extension);
  if (found == flavorTable().end()) {
    return Flavor::generic;
  }
  return found->second;
}

inline AffinityKey forPath(const std::filesystem::path& path) {
  return affinityOf(classify(path));
}

}
