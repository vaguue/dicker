#pragma once

#include <set>
#include <memory>
#include <algorithm>
#include <filesystem>

#include "protocol.hpp"
#include "compressor.h"
#include "conn.h"
#include "task.h"
#include "storage.h"
#include "worker.h"
#include "affinity.h"

namespace fs = std::filesystem;

struct Config {
  size_t diskConcurrency = 1;
  size_t workers = 4;
  size_t bigFileThreshold = 256 * 1024 * 1024;

  dicker::CompressionAlgo compressionAlgo;

  Conn conn;
};

struct Scheduler {
  Config cfg;
  std::shared_ptr<Storage> storage;
  std::vector<fs::path> roots;
  std::vector<std::unique_ptr<Worker>> workers;
  std::vector<std::shared_ptr<StorageReader>> readers;

  const uint32_t busyThreshold = 32;

  Scheduler(const Config& cfg) : cfg{cfg} {
    // One dedicated reader thread per worker (SPSC storage channels). Disk
    // parallelism is bounded independently by the shared Storage semaphore
    // (diskConcurrency), which the readers acquire before each read — so the
    // thread count and the concurrent-read cap are decoupled.
    this->storage = std::make_shared<Storage>(this->cfg.diskConcurrency);

    readers.reserve(this->cfg.workers);

    for (size_t i = 0; i < this->cfg.workers; ++i) {
      readers.emplace_back(std::make_shared<StorageReader>(this->storage.get()));
    }

    workers.reserve(cfg.workers);

    for (size_t i = 0; i < cfg.workers; ++i) {
      workers.emplace_back(std::make_unique<Worker>(
        makeCompressor(cfg.compressionAlgo),
        readers[i],
        cfg.conn)
      );
    }
  }

  size_t selectWorker(AffinityKey incoming) {
    size_t chosen = 0;

    size_t minIdx = 0;
    size_t minCap = 128;

    uint32_t chosenDistance = affinity::kWallCost + 1;

    for (int i{}; i < this->workers.size(); ++i) {
      auto& worker = this->workers[i];

      const wCap = worker.occupancy();

      if (wCap < minCap) {
        minIdx = i;
        minCap = wCap;
      }

      if (wCap >= busyThreshold) {
        continue;
      }

      const AffinityKey resident = worker.warmKey();

      const uint32_t distance = affinity::distance(incoming, resident);

      if (distance < chosenDistance) {
        chosenDistance = distance;
        chosen = i;
      }
    }

    return chosen;
  }

  void start() {
    // One shadow copy over the backup volume, before any reads. Falls back to
    // live reads if VSS is unavailable (see Storage::snapshot).
    if (!this->roots.empty()) {
      this->storage->snapshot(this->roots.front().string().c_str());
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

  void run(fs::path root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      if (entry.is_regular_file()) {
        auto size = entry.file_size();
        if (size >= this->cfg.bigFileThreshold) {
          //TODO
        }
        else {
          this->workers[this->selectWorker(affinity::forPath(entry.path()))]
            .enqueue({ dicker::UnitType::UnitType, entry.path().c_str() });
        }
      }
    }
  }
};
