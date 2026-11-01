#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "dicker/dicker.h"
#include "muypoco/timers.h"

using namespace std::chrono_literals;

struct Progress {
  std::atomic<std::uint64_t> units{0};
  std::atomic<std::uint64_t> failed{0};
  std::atomic<std::uint64_t> skipped{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> compressedBytes{0};
};

Progress progress;

std::uint64_t megabytes(std::uint64_t bytes) {
  return bytes / (1024 * 1024);
}

void announceFiles() {
  dicker::events::on<"upload:started">([](const dicker::UploadStarted& upload) noexcept {
    if (upload.type == dicker::UnitType::File || upload.offset == 0) {
      std::printf("-> %.*s (%llu bytes)\n",
                  static_cast<int>(upload.path.size()), upload.path.data(),
                  static_cast<unsigned long long>(upload.fileSize));
    }
  });
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <dicker-url> <root> [root...]\n", argv[0]);
    std::fprintf(stderr, "  e.g. %s dickers://secret@127.0.0.1:5959 ~/Documents\n", argv[0]);
    return 2;
  }

  dicker::events.on<"upload:finished">([](const dicker::UploadFinished& upload) noexcept {
    progress.units++;
    progress.bytes += upload.length;
    progress.compressedBytes += upload.compressedBytes;

    if (!upload.ok) {
      progress.failed++;
      std::printf("!! %.*s failed\n", static_cast<int>(upload.path.size()), upload.path.data());
    }
  });

  dicker::events.on<"upload:skipped">([](const dicker::UploadSkipped&) noexcept {
    progress.skipped++;
  });

  const int reporter = muypoco::setInterval([]() noexcept {
    std::printf("   %llu units, %llu MB read, %llu MB sent\n",
                static_cast<unsigned long long>(progress.units.load()),
                static_cast<unsigned long long>(megabytes(progress.bytes.load())),
                static_cast<unsigned long long>(megabytes(progress.compressedBytes.load())));
  }, 1s);

  dicker::Conn conn{argv[1]};
  dicker::Config config{conn};

  {
    dicker::Scheduler scheduler{config};

    for (int i = 2; i < argc; i++) {
      scheduler.addRoot(argv[i]);
    }
    scheduler.start();
  }

  muypoco::clearInterval(reporter);
  muypoco::wait();

  std::printf("done: %llu units, %llu skipped by the server, %llu failed, %llu MB read, %llu MB sent\n",
              static_cast<unsigned long long>(progress.units.load()),
              static_cast<unsigned long long>(progress.skipped.load()),
              static_cast<unsigned long long>(progress.failed.load()),
              static_cast<unsigned long long>(megabytes(progress.bytes.load())),
              static_cast<unsigned long long>(megabytes(progress.compressedBytes.load())));

  return progress.failed.load() == 0 ? 0 : 1;
}
