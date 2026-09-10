#pragma once

#include <set>
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

namespace fs = std::filesystem;

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
  std::vector<fs::path> roots;
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

  void addRoot(fs::path p) {
    roots.push_back(p);
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
      if (!entry.is_regular_file()) {
        continue;
      }

      const uint64_t size = entry.file_size();
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
