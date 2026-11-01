#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include "json.hpp"
#include "http_context.h"
#include "server.h"
#include "db.h"

namespace dicker {
  namespace controllers {

    namespace {

      // A session id names a directory under root; keep it to lowercase hex so
      // it can never escape the root or reference a parent.
      bool valid_session_id(const std::string& id) {
        if (id.empty() || id.size() > 64) {
          return false;
        }
        for (char c : id) {
          bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
          if (!hex) {
            return false;
          }
        }
        return true;
      }

    }  // namespace

    void uploads_list(HttpContext& ctx) {
      // Straight from the append-only session log — no disk walk. A session
      // whose directory has since disappeared simply errors at download time.
      std::map<std::string, SessionInfo> sessions;
      ctx.server().load_sessions(sessions);

      // ordered_json keeps the key order stable on the wire.
      nlohmann::ordered_json array = nlohmann::ordered_json::array();
      for (const auto& entry : sessions) {
        nlohmann::ordered_json row;
        row["sessionId"] = entry.first;
        row["ip"] = entry.second.ip;
        row["created"] = entry.second.created;
        array.push_back(std::move(row));
      }

      ctx.type = "application/json";
      ctx.body = array.dump();
    }

    void uploads_download(HttpContext& ctx) {
      const std::string& session_id = ctx.param;
      if (!valid_session_id(session_id)) {
        ctx.status = 404;
        ctx.body = "not found\n";
        return;
      }

      std::filesystem::path dir = ctx.server().config().root / session_id;
      std::error_code ec;
      if (!std::filesystem::is_directory(dir, ec)) {
        ctx.status = 404;
        ctx.body = "not found\n";
        return;
      }

      ctx.begin_download(session_id, dir.string());
    }

  }  // namespace controllers
}
