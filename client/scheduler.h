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

// rsync-style path filter: an ordered list of include/exclude rules; the FIRST
// rule that matches a path decides; no match => included. Semantics mirror rsync
// so unix users aren't surprised:
//   *            matches within a path segment (does not cross '/')
//   **           matches across '/'
//   ?            one non-'/' char
//   leading '/'  anchors the pattern to the root
//   trailing '/' matches directories only
//   no '/' in the pattern => matched against the basename at any depth
// To keep one file inside an otherwise-excluded dir, exclude the dir's CONTENTS
// (dir/**) — not the dir (dir/) — and put the re-include BEFORE it, e.g.
//   include "media_cache/version"  then  exclude "media_cache/**".
// Excluding the directory itself (dir/ or bare dir) prunes it (never walked).
struct FilterRule {
  bool include;
  std::string pattern;
};

// Sugar so callers write ordered rules readably:
//   addRoot(path, { include("media_cache/version"), exclude("media_cache/**") });
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
      re += ".*";        // ** crosses '/'
      i += 2;
    }
    else if (c == '*') {
      re += "[^/]*";     // * stays within a segment
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

struct Scheduler {
  Config cfg;
  std::shared_ptr<Storage> storage;
  std::vector<Root> roots;
  std::vector<std::unique_ptr<Worker>> workers;
  std::vector<std::shared_ptr<StorageReader>> readers;

  const uint32_t busyThreshold = 32;

  Scheduler(const Config& cfg) : cfg{cfg} {
    log().configure(this->cfg);

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
    size_t chosen = -1;

    size_t minIdx = 0;
    size_t minCap = 128;

    uint32_t chosenDistance = affinity::kWallCost + 1;

    for (size_t i{}; i < this->workers.size(); ++i) {
      auto& worker = this->workers[i];

      if (!worker->healthy()) {
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
    log().info("all units enqueued");
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
        if (!keep(root.rules, rel, name, true)) {
          it.disable_recursion_pending();
        }
        continue;
      }

      if (!entry.is_regular_file(ec)) {
        continue;
      }
      if (!keep(root.rules, rel, name, false)) {
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

          log().info("enqueue chunk: %s", path.c_str());
          auto& worker = this->workers[n % this->workers.size()];

          if (!worker->healthy()) {
            log().error("No healthy workers");
            return;
          }

          worker->enqueue(
            WorkerTask{ dicker::UnitType::Chunk, path.c_str(),
                        static_cast<size_t>(offset), static_cast<size_t>(len) });
        }
      }
      else {
        log().info("enqueue file: %s", path.c_str());

        auto& worker = this->workers[this->selectWorker(static_cast<AffinityKey>(affinity::forPath(entry.path())))];

        if (!worker->healthy()) {
          log().error("No healthy workers");
          return;
        }

        worker->enqueue(
          WorkerTask{ dicker::UnitType::File, path.c_str(), 0, static_cast<size_t>(size) });
      }
    }

    log().info("root done: %s", root.path.string().c_str());
  }
};
}  // namespace dicker
