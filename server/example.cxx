#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

#include "dicker/http_context.h"
#include "dicker/server.h"
#include "dicker/server_events.h"

struct Stats {
  std::atomic<std::uint64_t> clients{0};
  std::atomic<std::uint64_t> uploads{0};
  std::atomic<std::uint64_t> failed{0};
  std::atomic<std::uint64_t> skipped{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> compressedBytes{0};
};

Stats stats;

struct Seen {
  std::mutex lock;
  std::unordered_set<std::string> units;

  bool add(const dicker::server::Probe& probe) {
    std::string key(probe.path);
    key += '\0';
    key += std::to_string(probe.offset);
    key += '\0';
    key.append(reinterpret_cast<const char*>(probe.hash.data()), probe.hash.size());

    std::lock_guard<std::mutex> guard(this->lock);
    return this->units.emplace(std::move(key)).second;
  }
};

Seen seen;

std::string number(const std::atomic<std::uint64_t>& value) {
  return std::to_string(value.load());
}

void logConnections() {
  dicker::server::events::on<"client:connected">([](const dicker::server::ClientConnected& client) noexcept {
    std::fprintf(stderr, "+ %.*s session=%.*s %.*s%s\n",
                 static_cast<int>(client.peer.size()), client.peer.data(),
                 static_cast<int>(client.session.size()), client.session.data(),
                 static_cast<int>(client.compression.size()), client.compression.data(),
                 client.tls ? " tls" : "");
  });

  dicker::server::events::on<"client:disconnected">([](const dicker::server::ClientDisconnected& client) noexcept {
    std::fprintf(stderr, "- %.*s %llu units\n",
                 static_cast<int>(client.peer.size()), client.peer.data(),
                 static_cast<unsigned long long>(client.units));
  });
}

int main(int argc, char** argv) {
  const char* key = std::getenv("DICKER_KEY");

  if (argc < 3 || key == nullptr) {
    std::fprintf(stderr, "usage: DICKER_KEY=secret %s <root> <port>\n", argv[0]);
    return 2;
  }

  dicker::server::events.on<"client:connected">([](const dicker::server::ClientConnected&) noexcept {
    stats.clients++;
  });

  dicker::server::events.on<"upload:finished">([](const dicker::server::UploadFinished& upload) noexcept {
    stats.uploads++;
    stats.bytes += upload.bytes;
    stats.compressedBytes += upload.compressedBytes;

    if (!upload.ok) {
      stats.failed++;
      std::fprintf(stderr, "! %.*s: %.*s\n",
                   static_cast<int>(upload.path.size()), upload.path.data(),
                   static_cast<int>(upload.reason.size()), upload.reason.data());
    }
  });

  dicker::server::events.on<"upload:skipped">([](const dicker::server::UploadSkipped&) noexcept {
    stats.skipped++;
  });

  dicker::ServerConfig config;
  config.key = key;
  config.root = argv[1];

  dicker::Server server(config);

  server.filter([](const dicker::server::Probe& probe) {
    return seen.add(probe);
  }, dicker::server::ProbeHash::blake3);

  server.route("GET", "/health", [](dicker::HttpContext& ctx) {
    ctx.body = "ok\n";
  });

  server.route("GET", "/stats", [](dicker::HttpContext& ctx) {
    ctx.type = "application/json";
    ctx.body = "{\"clients\":" + number(stats.clients) +
               ",\"uploads\":" + number(stats.uploads) +
               ",\"failed\":" + number(stats.failed) +
               ",\"skipped\":" + number(stats.skipped) +
               ",\"bytes\":" + number(stats.bytes) +
               ",\"compressedBytes\":" + number(stats.compressedBytes) + "}\n";
  });

  server.route("GET", dicker::Match::Prefix, "/hello/", [](dicker::HttpContext& ctx) {
    ctx.body = "hello, " + ctx.param + "\n";
  });

  server.route("POST", "/stop", [&server](dicker::HttpContext& ctx) {
    ctx.body = "stopping\n";
    server.stop();
  }, {dicker::mw::require_auth});

  const int port = std::atoi(argv[2]);

  if (!server.listen("0.0.0.0", port)) {
    std::fprintf(stderr, "failed to listen on port %d\n", port);
    return 1;
  }

  std::fprintf(stderr, "example server on :%d, try /health, /stats, /hello/you, POST /stop\n", port);
  server.run();
  std::fprintf(stderr, "stopped\n");
  return 0;
}
