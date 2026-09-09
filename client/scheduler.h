#pragma once

#include <set>
#include <memory>
#include <algorithm>
#include <filesystem>

#include "protocol.hpp"
#include "compressor.h"
#include "conn.h"
#include "task.h"
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
  std::vector<fs::path> roots;
  std::vector<std::unique_ptr<Worker>> workers;
  std::vector<std::shared_ptr<StorageReader>> readers;

  const uint32_t busyThreshold = 32;

  Scheduler(const Config& cfg) : cfg{cfg} {
    // SPSC storage channels: each reader may be fed by exactly one worker, so
    // there must be at least as many readers as workers (a reader is dedicated,
    // not shared). diskConcurrency below workers cannot bound disk IO by sharing
    // a reader queue — that needs a shared limiter, decided separately.
    size_t readerCount = std::max(cfg.diskConcurrency, cfg.workers);

    readers.reserve(readerCount);

    for (size_t i = 0; i < readerCount; ++i) {
      readers.emplace_back(std::make_shared<StorageReader>());
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
