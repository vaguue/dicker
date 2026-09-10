#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dicker.h"

namespace {

void usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: DICKER_KEY=secret %s <dicker-url> [options] --root <path> [--include <glob>]... [--exclude <glob>]... [--root ...]...\n"
    "  url                          dicker://[sessionId:]secret@host:port?query\n"
    "                               (query: integrity, compressionAlgo=zstd|lz4|none)\n"
    "  --root <path>                a directory to back up (repeatable)\n"
    "  --include <glob>             only upload files matching this glob (per --root, repeatable)\n"
    "  --exclude <glob>             skip files/dirs matching this glob (per --root, repeatable)\n"
    "  --workers <n>                parallel upload workers/connections (default 4)\n"
    "  --disk-concurrency <n>       max simultaneous disk reads (default 1)\n"
    "  --big-file-threshold <SIZE>  split files >= SIZE into chunks (default 256M)\n"
    "  --chunk-size <SIZE>          chunk size for big files (default 64M)\n"
    "  SIZE accepts a K/M/G suffix, e.g. 256M\n",
    argv0);
}

std::size_t parseSize(const char* s) {
  char* end = nullptr;
  double v = std::strtod(s, &end);
  std::size_t mult = 1;
  if (end != nullptr && *end != '\0') {
    switch (*end) {
      case 'k': case 'K': mult = 1024ull; break;
      case 'm': case 'M': mult = 1024ull * 1024; break;
      case 'g': case 'G': mult = 1024ull * 1024 * 1024; break;
      default: break;
    }
  }
  return static_cast<std::size_t>(v * static_cast<double>(mult));
}

struct PendingRoot {
  std::string path;
  std::vector<std::string> include;
  std::vector<std::string> exclude;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argv[1][0] == '-') {
    usage(argv[0]);
    return 2;
  }

  dicker::Conn conn{std::string(argv[1])};
  if (const char* key = std::getenv("DICKER_KEY")) {
    conn.key = key;
  }
  if (conn.key.empty()) {
    std::fprintf(stderr, "no key: set DICKER_KEY or put the secret in the url\n");
    return 2;
  }

  dicker::Config cfg{conn};
  std::vector<PendingRoot> roots;

  for (int i = 2; i < argc; i += 2) {
    const std::string arg = argv[i];
    if (i + 1 >= argc) {
      std::fprintf(stderr, "%s needs a value\n", arg.c_str());
      return 2;
    }
    const char* val = argv[i + 1];

    if (arg == "--root") {
      roots.push_back(PendingRoot{val, {}, {}});
    }
    else if (arg == "--include" || arg == "--exclude") {
      if (roots.empty()) {
        std::fprintf(stderr, "%s must follow a --root\n", arg.c_str());
        return 2;
      }
      (arg == "--include" ? roots.back().include : roots.back().exclude).push_back(val);
    }
    else if (arg == "--workers") {
      cfg.workers = std::strtoul(val, nullptr, 10);
    }
    else if (arg == "--disk-concurrency") {
      cfg.diskConcurrency = std::strtoul(val, nullptr, 10);
    }
    else if (arg == "--big-file-threshold") {
      cfg.bigFileThreshold = parseSize(val);
    }
    else if (arg == "--chunk-size") {
      cfg.chunkSize = parseSize(val);
    }
    else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      usage(argv[0]);
      return 2;
    }
  }

  if (roots.empty()) {
    std::fprintf(stderr, "no --root given\n");
    return 2;
  }
  if (cfg.workers < 1) {
    std::fprintf(stderr, "--workers must be >= 1\n");
    return 2;
  }

  dicker::Scheduler dicker{cfg};
  for (const PendingRoot& r : roots) {
    dicker.addRoot(r.path, r.include, r.exclude);
  }
  dicker.start();
  return 0;
}
