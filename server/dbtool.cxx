// dicker-dbtool — offline management of the server's encrypted token store
// (server/db.h). Runs on the server host, next to the systemd unit; it needs the
// same DICKER_KEY the server runs with (e.g. the unit's Environment=/
// EnvironmentFile=).
//
//   DICKER_KEY=... dicker-dbtool --root <dir> add-token <token>
//   DICKER_KEY=... dicker-dbtool --root <dir> del-token <token>
//   DICKER_KEY=... dicker-dbtool --root <dir> list-tokens
//   DICKER_KEY=... dicker-dbtool --root <dir> sessions
//
// --root must match the server's --root. Tokens live in <root>/.dicker-tokens.json
// (dbtool is the only writer; changes take effect on the server live, no restart).
// Sessions live in <root>/.dicker-sessions.jsonl (read-only here).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <filesystem>

#include "db.h"

namespace {

  bool matches(const char* argument, const char* name) {
    return std::strcmp(argument, name) == 0;
  }

  void usage() {
    std::fprintf(stderr,
                 "usage: dicker-dbtool --root <dir> [--key <secret>] <command>\n"
                 "  commands: add-token <token> | del-token <token> | "
                 "list-tokens | sessions\n"
                 "  key defaults to $DICKER_KEY\n");
  }

}  // namespace

int main(int argc, char** argv) {
  std::string root;
  std::string key;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    bool has_value = i + 1 < argc;
    if (matches(arg, "--root") && has_value) {
      root = argv[++i];
    }
    else if (matches(arg, "--key") && has_value) {
      key = argv[++i];
    }
    else if (arg[0] == '-') {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n", arg);
      usage();
      return 1;
    }
    else {
      positional.emplace_back(arg);
    }
  }

  if (key.empty()) {
    const char* env = std::getenv("DICKER_KEY");
    if (env != nullptr) {
      key = env;
    }
  }

  if (root.empty() || key.empty() || positional.empty()) {
    usage();
    return 1;
  }

  std::filesystem::path root_path(root);
  std::string tokens_path = (root_path / ".dicker-tokens.json").string();
  std::string sessions_path = (root_path / ".dicker-sessions.jsonl").string();
  const std::string& command = positional[0];

  if (command == "sessions") {
    std::map<std::string, dicker::SessionInfo> sessions;
    dicker::SessionLog::load_all(sessions_path, key, sessions);
    for (const auto& entry : sessions) {
      std::printf("%s\t%s\t%llu\n", entry.first.c_str(), entry.second.ip.c_str(),
                  static_cast<unsigned long long>(entry.second.created));
    }
    return 0;
  }

  // Remaining commands operate on the token store.
  dicker::TokenStore tokens;
  std::string err;
  if (!dicker::TokenStore::load(tokens_path, key, tokens, err)) {
    std::fprintf(stderr, "load %s: %s\n", tokens_path.c_str(), err.c_str());
    return 1;
  }

  auto save = [&]() -> int {
    std::string save_err;
    if (!tokens.save(tokens_path, key, save_err)) {
      std::fprintf(stderr, "save %s: %s\n", tokens_path.c_str(), save_err.c_str());
      return 1;
    }
    return 0;
  };

  if (command == "list-tokens") {
    for (const std::string& token : tokens.tokens) {
      std::printf("%s\n", token.c_str());
    }
    return 0;
  }

  if (command == "add-token") {
    if (positional.size() < 2) {
      usage();
      return 1;
    }
    const std::string& token = positional[1];
    if (std::find(tokens.tokens.begin(), tokens.tokens.end(), token) !=
        tokens.tokens.end()) {
      std::fprintf(stderr, "token already present\n");
      return 0;
    }
    tokens.tokens.push_back(token);
    if (save() != 0) {
      return 1;
    }
    std::fprintf(stderr, "added token (%zu total)\n", tokens.tokens.size());
    return 0;
  }

  if (command == "del-token") {
    if (positional.size() < 2) {
      usage();
      return 1;
    }
    const std::string& token = positional[1];
    std::size_t before = tokens.tokens.size();
    tokens.tokens.erase(
        std::remove(tokens.tokens.begin(), tokens.tokens.end(), token),
        tokens.tokens.end());
    if (tokens.tokens.size() == before) {
      std::fprintf(stderr, "token not found\n");
      return 0;
    }
    if (save() != 0) {
      return 1;
    }
    std::fprintf(stderr, "removed token (%zu total)\n", tokens.tokens.size());
    return 0;
  }

  std::fprintf(stderr, "unknown command: %s\n", command.c_str());
  usage();
  return 1;
}
