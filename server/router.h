#pragma once

#include "http.h"
#include "middleware.h"

namespace dicker {

  struct Connection;
  struct HttpContext;

  using HttpHandler = void (*)(HttpContext&);

  enum class Match {
    Exact,   // request.path == path
    Prefix,  // request.path starts with path; the tail is exposed as ctx.param
  };

  // One row of the compile-time route table (see router.cxx). Handlers and
  // middleware each live in their own file, Express/Koa style.
  struct Route {
    const char* method;   // "GET", "POST", ...
    Match match;
    const char* path;
    Chain middleware;     // runs before the handler; {} for none
    HttpHandler handler;
  };

  // Match `request` against the route table and dispatch it through the route's
  // middleware chain, writing the reply via `conn`. Unmatched path -> 404;
  // matched path, wrong method -> 405.
  void dispatch_http(Connection& conn, const HttpRequest& request);

}
