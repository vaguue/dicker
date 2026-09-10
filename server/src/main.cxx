#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <uv.h>
#include "server.h"

namespace {

  constexpr std::size_t kDefaultHighWater = 8u * 1024 * 1024;
  constexpr std::size_t kDefaultLowWater = 2u * 1024 * 1024;
  constexpr int kDefaultPort = 5959;

  struct Options {
    std::string listen_address = "0.0.0.0";
    int port = kDefaultPort;
    std::string root;
    std::string key;
    std::size_t high_water = kDefaultHighWater;
    std::size_t low_water = kDefaultLowWater;
  };

  bool matches(const char* argument, const char* name) {
    return std::strcmp(argument, name) == 0;
  }

  bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
      const char* argument = argv[index];
      bool has_value = index + 1 < argc;

      if (matches(argument, "--listen") && has_value) {
        options.listen_address = argv[++index];
      }
      else if (matches(argument, "--port") && has_value) {
        options.port = std::atoi(argv[++index]);
      }
      else if (matches(argument, "--root") && has_value) {
        options.root = argv[++index];
      }
      else if (matches(argument, "--key") && has_value) {
        options.key = argv[++index];
      }
      else if (matches(argument, "--high-water") && has_value) {
        options.high_water = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
      }
      else if (matches(argument, "--low-water") && has_value) {
        options.low_water = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
      }
      else {
        std::fprintf(stderr, "unknown or incomplete argument: %s\n", argument);
        return false;
      }
    }

    if (options.key.empty()) {
      const char* environment_key = std::getenv("DICKER_KEY");
      if (environment_key != nullptr) {
        options.key = environment_key;
      }
    }

    if (options.root.empty()) {
      std::fprintf(stderr, "missing required --root <directory>\n");
      return false;
    }
    if (options.key.empty()) {
      std::fprintf(stderr, "missing required --key <secret> or DICKER_KEY\n");
      return false;
    }
    return true;
  }

}

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 1;
  }

  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) {
    std::fprintf(stderr, "failed to initialize event loop\n");
    return 1;
  }

  dicker::ServerConfig config;
  config.key = options.key;
  config.root = options.root;
  config.channel_high_water = options.high_water;
  config.channel_low_water = options.low_water;

  dicker::Server server(&loop, std::move(config));
  if (!server.listen(options.listen_address, options.port)) {
    std::fprintf(stderr, "failed to listen on %s:%d\n", options.listen_address.c_str(), options.port);
    uv_loop_close(&loop);
    return 1;
  }

  std::fprintf(stderr, "dicker server listening on %s:%d root=%s\n",
               options.listen_address.c_str(), options.port, options.root.c_str());

  int result = uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
  return result;
}
