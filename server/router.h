#pragma once

#include <functional>
#include <string>
#include <vector>

#include "http.h"
#include "middleware.h"

namespace dicker {

  struct Connection;
  struct HttpContext;

  using HttpHandler = std::function<void(HttpContext&)>;

  enum class Match {
    Exact,   // request.path == path
    Prefix,  // request.path starts with path; the tail is exposed as ctx.param
  };

  struct Route {
    std::string method;   // "GET", "POST", ...
    Match match;
    std::string path;
    Chain middleware;     // runs before the handler; {} for none
    HttpHandler handler;
  };

  // Routes are checked in registration order: the server's own routes first,
  // then the built-in ones (/, /uploads, /uploads/...). Unmatched path -> 404;
  // matched path, wrong method -> 405.
  struct Router {
    std::vector<Route> routes;
    std::vector<Route> builtins;

    void add(std::string method, Match match, std::string path, HttpHandler handler, Chain middleware = {});
    void dispatch(Connection& conn, const HttpRequest& request) const;
  };

  void add_builtin_routes(Router& router);

  // Match `request` against the connection's server routes and dispatch it
  // through the route's middleware chain, writing the reply via `conn`.
  void dispatch_http(Connection& conn, const HttpRequest& request);

}
