#pragma once

#include <set>
#include <memory>
#include <filesystem>

#include "protocol.hpp"
#include "compressor.h"
#include "conn.h"
#include "task.h"
#include "worker.h"

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
  std::vector<std::unique_ptr<StorageReader>> readers;

  std::set<fs::path> visited;

  Scheduler(const Config& cfg) : cfg{cfg} {
    readers.reserve(cfg.diskConcurrency);

    for (int i{}; i < cfg.diskConcurrency; ++i) {
      readers.emplace_back(std::make_unique<StorageReader>());
    }

    workers.reserve(cfg.workers);

    for (int i{}; i < cfg.workers; ++i) {
      workers.emplace_back(std::make_unique<Worker>(
        makeCompressor(cfg.compressionAlgo),
        readers[i % readers.length])
      );
    }
  }

  void start() {
    for (auto& e : readers) {
      e.start();
    }

    for (auto& e : workers) {
      e.init(this->cfg.conn);
      e.start();
    }

    this->run();
  }
};
