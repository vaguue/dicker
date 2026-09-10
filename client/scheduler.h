#pragma once

#include <set>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <filesystem>

#include "protocol.h"
#include "compressor.h"
#include "conn.h"
#include "task.h"
#include "storage.h"
#include "worker.h"
#include "affinity.h"
#include "glob.h"

namespace dicker {
namespace fs = std::filesystem;

// A backup root plus optional glob filters. include: if non-empty, only files
// matching one of these are uploaded. exclude: files/dirs matching one of these
// are skipped — and a matching directory is pruned (not descended into), so e.g.
// the media cache is never even walked. Patterns are matched (glob::fnmatch,
// where '*' spans '/') against both the root-relative path and the basename.
struct Root {
  fs::path path;
  std::vector<std::string> include;
  std::vector<std::string> exclude;
};

inline bool matchesAny(const std::string& rel, const std::string& name,
                       const std::vector<std::string>& patterns) {
  for (const std::string& p : patterns) {
    if (glob::fnmatch(rel, p) || glob::fnmatch(name, p)) {
      return true;
    }
  }
  return false;
}

struct Config {
  Conn conn;

  size_t diskConcurrency = 1;
  size_t workers = 4;
  size_t bigFileThreshold = 256 * 1024 * 1024;
  size_t chunkSize = 64 * 1024 * 1024;
};

struct Scheduler {
  Config cfg;
  std::shared_ptr<Storage> storage;
  std::vector<Root> roots;
  std::vector<std::unique_ptr<Worker>> workers;
  std::vector<std::shared_ptr<StorageReader>> readers;

  const uint32_t busyThreshold = 32;

  Scheduler(const Config& cfg) : cfg{cfg} {
    this->storage = std::make_shared<Storage>(this->cfg.diskConcurrency);

    readers.reserve(this->cfg.workers);

    for (size_t i = 0; i < this->cfg.workers; ++i) {
      readers.emplace_back(std::make_shared<StorageReader>(this->storage.get()));
    }

    workers.reserve(cfg.workers);

    for (size_t i = 0; i < cfg.workers; ++i) {
      workers.emplace_back(std::make_unique<Worker>(
        makeCompressor(cfg.conn.compressionAlgo),
        readers[i],
        cfg.conn)
      );
    }
  }

  void addRoot(fs::path p,
               std::vector<std::string> include = {},
               std::vector<std::string> exclude = {}) {
    this->roots.push_back({ std::move(p), std::move(include), std::move(exclude) });
  }

  size_t selectWorker(AffinityKey incoming) {
    size_t chosen = -1;

    size_t minIdx = 0;
    size_t minCap = 128;

    uint32_t chosenDistance = affinity::kWallCost + 1;

    for (size_t i{}; i < this->workers.size(); ++i) {
      auto& worker = this->workers[i];

      auto wCap = worker->occupancy();

      if (wCap < minCap) {
        minIdx = i;
        minCap = wCap;
      }

      if (wCap >= busyThreshold) {
        continue;
      }

      const AffinityKey resident = worker->warmKey();

      const uint32_t distance = affinity::distance(static_cast<affinity::AffinityKey>(incoming), static_cast<affinity::AffinityKey>(resident));

      if (distance < chosenDistance) {
        chosenDistance = distance;
        chosen = i;
      }
    }

    if (chosen != static_cast<size_t>(-1)) {
      return chosen;
    }

    return minIdx;
  }

  void start() {
    if (!this->roots.empty()) {
      this->storage->snapshot(this->roots.front().path.string().c_str());
    }

    for (auto& e : readers) {
      e->start();
    }

    for (auto& e : workers) {
      e->start();
    }

    for (auto& e : roots) {
      this->run(e);
    }
  }

  void run(const Root& root) {
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(
      root.path, fs::directory_options::skip_permission_denied, ec);
    const auto end = fs::recursive_directory_iterator();

    for (; !ec && it != end; it.increment(ec)) {
      const fs::directory_entry& entry = *it;
      const std::string rel = fs::relative(entry.path(), root.path).generic_string();
      const std::string name = entry.path().filename().string();

      if (entry.is_directory(ec)) {
        // prune an excluded directory: skip its whole subtree (e.g. media cache)
        if (matchesAny(rel, name, root.exclude)) {
          it.disable_recursion_pending();
        }
        continue;
      }

      if (!entry.is_regular_file(ec)) {
        continue;
      }
      if (matchesAny(rel, name, root.exclude)) {
        continue;
      }
      if (!root.include.empty() && !matchesAny(rel, name, root.include)) {
        continue;
      }

      const uint64_t size = entry.file_size(ec);
      if (ec) {
        continue;
      }
      const std::string path = entry.path().string();

      if (size >= this->cfg.bigFileThreshold) {
        const uint64_t chunkSize = this->cfg.chunkSize;
        size_t n = 0;

        for (uint64_t offset = 0; offset < size; offset += chunkSize, ++n) {
          const uint64_t len = std::min<uint64_t>(chunkSize, size - offset);

          this->workers[n % this->workers.size()]->enqueue(
            WorkerTask{ dicker::UnitType::Chunk, path.c_str(),
                        static_cast<size_t>(offset), static_cast<size_t>(len) });
        }
      }
      else {
        this->workers[this->selectWorker(static_cast<AffinityKey>(affinity::forPath(entry.path())))]->enqueue(
          WorkerTask{ dicker::UnitType::File, path.c_str(), 0, static_cast<size_t>(size) });
      }
    }
  }
};
}  // namespace dicker
