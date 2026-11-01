#include "router.h"

#include <cstring>
#include <string>
#include <utility>
#include "connection.h"
#include "http_context.h"
#include "server.h"

namespace dicker {

  // Controllers and middleware are each defined in their own .cxx file; the
  // only place that needs their declarations is the built-in route list below,
  // so we forward-declare them here rather than carry a header per file.
  namespace controllers {
    void root(HttpContext&);
    void uploads_list(HttpContext&);
    void uploads_download(HttpContext&);
  }
  namespace {

    bool path_matches(const Route& route, const std::string& path) {
      if (route.match == Match::Exact) {
        return path == route.path;
      }
      return path.size() >= route.path.size() && path.compare(0, route.path.size(), route.path) == 0;
    }

    const Route* find(const std::vector<Route>& routes, const HttpRequest& request, bool& path_matched) {
      for (const Route& route : routes) {
        if (!path_matches(route, request.path)) {
          continue;
        }
        path_matched = true;
        if (request.method == route.method) {
          return &route;
        }
      }
      return nullptr;
    }

  }  // namespace

  void add_builtin_routes(Router& router) {
    router.builtins.push_back({"GET", Match::Exact, "/", {}, controllers::root});
    router.builtins.push_back({"GET", Match::Exact, "/uploads", {mw::require_auth}, controllers::uploads_list});
    router.builtins.push_back({"GET", Match::Prefix, "/uploads/", {mw::require_auth}, controllers::uploads_download});
  }

  void Router::add(std::string method, Match match, std::string path, HttpHandler handler, Chain middleware) {
    this->routes.push_back({std::move(method), match, std::move(path), std::move(middleware), std::move(handler)});
  }

  void Next::operator()(HttpContext& ctx) {
    if (index < route.middleware.size()) {
      Middleware current = route.middleware[index];
      Next onward{route, conn, index + 1};
      current(ctx, onward);
    }
    else {
      route.handler(ctx);
    }
  }

  void Router::dispatch(Connection& conn, const HttpRequest& request) const {
    bool path_matched = false;
    const Route* matched = find(this->routes, request, path_matched);

    if (matched == nullptr) {
      matched = find(this->builtins, request, path_matched);
    }

    if (matched == nullptr) {
      if (path_matched) {
        conn.send_http_simple(405, "Method Not Allowed", "text/plain; charset=utf-8",
                              "method not allowed\n");
      }
      else {
        conn.send_http_simple(404, "Not Found", "text/plain; charset=utf-8",
                              "not found\n");
      }
      return;
    }

    std::string param;
    if (matched->match == Match::Prefix) {
      param = request.path.substr(matched->path.size());
    }

    HttpContext ctx(conn, request, std::move(param));
    Next next{*matched, conn, 0};
    next(ctx);  // runs the middleware chain, then the handler

    if (ctx.respond) {
      conn.send_http_simple(ctx.status, reason_phrase(ctx.status), ctx.type,
                            ctx.body, ctx.headers);
    }
  }

  void dispatch_http(Connection& conn, const HttpRequest& request) {
    conn.server_->router.dispatch(conn, request);
  }

}
