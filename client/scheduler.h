#pragma once

#include <set>
#include <string>
#include <vector>
#include <memory>
#include <regex>
#include <cstring>
#include <algorithm>
#include <filesystem>

#include "protocol.h"
#include "compressor.h"
#include "conn.h"
#include "config.h"
#include "task.h"
#include "storage.h"
#include "worker.h"
#include "affinity.h"
#include "log.h"

namespace dicker {
namespace fs = std::filesystem;

inline std::string toUtf8(const fs::path& p) {
#if defined(__cpp_char8_t)
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
#else
  return p.u8string();
#endif
}

inline std::string toGenericUtf8(const fs::path& p) {
#if defined(__cpp_char8_t)
  const auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
#else
  return p.generic_u8string();
#endif
}

struct FilterRule {
  bool include;
  std::string pattern;
};

inline FilterRule include(std::string pattern) { return FilterRule{ true, std::move(pattern) }; }
inline FilterRule exclude(std::string pattern) { return FilterRule{ false, std::move(pattern) }; }

struct Rule {
  bool include;
  bool dirOnly;
  bool basenameOnly;
  std::regex re;
};

inline Rule compileRule(const FilterRule& fr) {
  std::string pat = fr.pattern;
  Rule rule;
  rule.include = fr.include;
  rule.dirOnly = false;

  if (!pat.empty() && pat.back() == '/') {
    rule.dirOnly = true;
    pat.pop_back();
  }
  bool anchored = false;
  if (!pat.empty() && pat.front() == '/') {
    anchored = true;
    pat.erase(0, 1);
  }
  rule.basenameOnly =
    pat.find('/') == std::string::npos && pat.find("**") == std::string::npos;

  std::string re;
  for (std::size_t i = 0; i < pat.size();) {
    char c = pat[i];
    if (c == '*' && i + 1 < pat.size() && pat[i + 1] == '*') {
      re += ".*";
      i += 2;
    }
    else if (c == '*') {
      re += "[^/]*";
      i += 1;
    }
    else if (c == '?') {
      re += "[^/]";
      i += 1;
    }
    else {
      if (std::strchr(".^$+{}()[]|\\", c) != nullptr) {
        re += '\\';
      }
      re += c;
      i += 1;
    }
  }

  const std::string full = (rule.basenameOnly || anchored) ? re : ("(.*/)?" + re);
  rule.re = std::regex("^" + full + "$");
  return rule;
}

// First-match-wins; default keep.
inline bool keep(const std::vector<Rule>& rules, const std::string& rel,
                 const std::string& name, bool isDir) {
  for (const Rule& r : rules) {
    if (r.dirOnly && !isDir) {
      continue;
    }
    if (std::regex_match(r.basenameOnly ? name : rel, r.re)) {
      return r.include;
    }
  }
  return true;
}

struct Root {
  fs::path path;
  std::vector<Rule> rules;
};

struct Scheduler { //TODO add a stop method
  enum class WorkerState : uint8_t {
    PENDING = 0,
    READY = 1,
    FAILED = 2,
  };

  Config cfg;
  std::shared_ptr<Storage> storage;
  std::vector<Root> roots;
  std::vector<std::shared_ptr<StorageReaderChan>> readers;
  std::vector<std::unique_ptr<WorkerChan>> workers;
  std::vector<WorkerState> workersStates;

  const uint32_t busyThreshold = 32;

  Scheduler(const Config& cfg) : cfg{cfg} {
    log().configure(this->cfg);

    this->storage = std::make_shared<Storage>(this->cfg.diskConcurrency);

    this->readers.reserve(this->cfg.workers);

    for (size_t i = 0; i < this->cfg.workers; ++i) {
      this->readers.emplace_back(std::make_shared<StorageReaderChan>(this->storage.get()));
    }

    this->workers.reserve(cfg.workers);
    this->workersStates.resize(cfg.workers);

    for (size_t i = 0; i < cfg.workers; ++i) {
      this->workers.emplace_back(std::make_unique<WorkerChan>(
        makeCompressor(cfg.conn.compressionAlgo),
        this->readers[i],
        cfg.conn)
      );
    }
  }

  ~Scheduler() {
    this->workers.clear();

    for (auto& reader : this->readers) {
      reader->worker.close();
    }
  }

  void addRoot(fs::path p, std::vector<FilterRule> rules = {}) {
    Root root;
    root.path = std::move(p);
    root.rules.reserve(rules.size());

    for (const FilterRule& fr : rules) {
      root.rules.push_back(compileRule(fr));
    }

    this->roots.push_back(std::move(root));
  }

  size_t selectWorker(AffinityKey incoming) {
    size_t chosen = static_cast<size_t>(-1);

    size_t minIdx = static_cast<size_t>(-1);
    size_t minCap = 128;

    uint32_t chosenDistance = affinity::kWallCost + 1;

    for (size_t i{}; i < this->workers.size(); ++i) {
      auto& worker = this->workers[i];

      if (worker->isStopped()) {
        continue;
      }

      auto wCap = worker->occupancy();

      if (wCap < minCap) {
        minIdx = i;
        minCap = wCap;
      }

      if (wCap >= busyThreshold) {
        continue;
      }

      const AffinityKey resident = worker->worker.warm.load(std::memory_order_relaxed);

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

  bool dispatch(size_t startIdx, const WorkerTask& t) {
    for (size_t k = 0; k < this->workers.size(); ++k) {
      size_t i = (startIdx + k) % this->workers.size();
      auto& worker = this->workers[i];

      if (this->workersStates[i] == WorkerState::PENDING) {
        if (!worker->awaitInit()) {
          this->workersStates[i] = WorkerState::FAILED;
          log().info("Skipping worker (1) %zu", i);
          continue;
        }
        else {
          this->workersStates[i] = WorkerState::READY;
        }
      }

      if (workersStates[i] == WorkerState::FAILED || worker->isStopped()) {
        log().info("Skipping worker (2) %zu", i);
        continue;
      }
      if (worker->enqueue(t)) {
        log().info("Queueing worker %zu", i);
        return true;
      }
    }

    return false;
  }

  void start() {
    if (!this->roots.empty()) {
      this->storage->snapshot(toUtf8(this->roots.front().path).c_str());
    }

    for (auto& e : readers) {
      e->start();
    }

    for (auto& e : workers) {
      e->start();
    }

    bool ok = true;
    for (auto& e : roots) {
      if (!this->run(e)) {
        ok = false;
      }
    }
    if (ok) {
      log().info("all units enqueued");
    }
  }

  bool enqueue(const fs::directory_entry& entry) {
    const std::string path = toUtf8(entry.path());

    std::error_code ec;
    const uint64_t size = entry.file_size(ec);

    if (ec) {
      log().error("file_size failed, skipping: %s (%s)", path.c_str(), ec.message().c_str());
      return true;
    }

    if (size >= this->cfg.bigFileThreshold) {
      const uint64_t chunkSize = this->cfg.chunkSize;
      size_t n = 0;

      for (uint64_t offset = 0; offset < size; offset += chunkSize, ++n) {
        const uint64_t len = std::min<uint64_t>(chunkSize, size - offset);

        log().info("enqueue chunk: %s", path.c_str());

        WorkerTask task{ dicker::UnitType::Chunk, path.c_str(), static_cast<size_t>(offset), static_cast<size_t>(len), size };

        if (!this->dispatch(n % this->workers.size(), task)) {
          log().error("No healthy workers");
          return false;
        }
      }
    }
    else {
      log().info("enqueue file: %s", path.c_str());

      WorkerTask task{ dicker::UnitType::File, path.c_str(), 0, static_cast<size_t>(size), size };
      size_t i = this->selectWorker(static_cast<AffinityKey>(affinity::forPath(entry.path())));

      if (!this->dispatch(i, task)) {
        log().error("No healthy workers");
        return false;
      }
    }

    return true;
  }

  bool run(const Root& root) {
    const std::string rootStr = toUtf8(root.path);

    std::error_code iterEc;

    auto it = fs::recursive_directory_iterator(root.path, fs::directory_options::skip_permission_denied, iterEc);

    const auto end = fs::recursive_directory_iterator();

    for (; !iterEc && it != end; it.increment(iterEc)) {
      const fs::directory_entry& entry = *it;

      const std::string rel = toGenericUtf8(entry.path().lexically_relative(root.path));
      const std::string name = toUtf8(entry.path().filename());

      std::error_code ec;

      const bool isDir = entry.is_directory(ec);
      if (ec) {
        log().error("stat failed, skipping: %s (%s)", toUtf8(entry.path()).c_str(), ec.message().c_str());
        continue;
      }

      if (isDir) {
        if (!keep(root.rules, rel, name, true)) {
          it.disable_recursion_pending();
        }
        continue;
      }

      const bool isFile = entry.is_regular_file(ec);
      if (ec || !isFile) {
        continue;
      }

      if (!keep(root.rules, rel, name, false)) {
        continue;
      }

      if (!this->enqueue(entry)) {
        return false;
      }
    }

    if (iterEc) {
      log().error("walk aborted: %s (%s)", rootStr.c_str(), iterEc.message().c_str());
      return false;
    }

    log().info("root done: %s", rootStr.c_str());
    return true;
  }
};
}  // namespace dicker
