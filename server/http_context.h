#pragma once

#include <string>
#include <utility>
#include <vector>
#include "http.h"

namespace dicker {

  struct Connection;
  struct Server;

  // Koa-style per-request context handed to every endpoint handler.
  //
  // A handler reads the request off `req` (plus `param`, the captured tail of a
  // prefix route) and builds its reply by setting `status`, `type`, `body`, and
  // any extra `headers`; the router flushes that after the handler returns. A
  // handler that instead streams its own response (e.g. an archive download)
  // calls a takeover helper such as begin_download(), which flips `respond` off
  // so the router leaves the socket alone.
  struct HttpContext {
    HttpContext(Connection& conn, const HttpRequest& req, std::string param)
      : req(req), param(std::move(param)), conn(conn) {}

    // ---- request ----
    const HttpRequest& req;
    std::string param;

    const std::string* header(const std::string& lower_name) const {
      return req.header(lower_name);
    }

    // ---- response (set these; the router sends them) ----
    int status = 200;
    std::string type = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool respond = true;

    void set(std::string name, std::string value) {
      headers.emplace_back(std::move(name), std::move(value));
    }

    // ---- environment / takeovers ----
    Server& server() const;

    // Hand the socket to Connection's chunked archive streamer and stop the
    // router from writing a buffered response for this request.
    void begin_download(const std::string& session_id, const std::string& dir_path);

    Connection& conn;
  };

}
