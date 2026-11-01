#include "router.h"

#include <cstring>
#include <string>
#include "connection.h"
#include "http_context.h"

namespace dicker {

  // Controllers and middleware are each defined in their own .cxx file; the
  // only place that needs their declarations is the route table below, so we
  // forward-declare them here rather than carry a header per file.
  namespace controllers {
    void root(HttpContext&);
    void task(HttpContext&);
    void uploads_list(HttpContext&);
    void uploads_download(HttpContext&);
  }
  namespace mw {
    void require_auth(HttpContext&, Next&);
  }

  namespace {

    // Reusable middleware chains. Declare one constexpr array per chain and hand
    // it to the route rows below; a route with no middleware gets {}.
    constexpr Middleware kAuth[] = {mw::require_auth};

    // The compile-time route table. To add an endpoint: create a
    // controllers/<name>.{h,cxx} exposing a handler, include its header above,
    // and add one row here. Order only matters between rows that could match
    // the same path (none do today).
    constexpr Route kRoutes[] = {
      {"GET", Match::Exact, "/", {}, controllers::root},
      {"GET", Match::Exact, "/uploads", kAuth, controllers::uploads_list},
      {"GET", Match::Prefix, "/uploads/", kAuth, controllers::uploads_download},
    };

    bool path_matches(const Route& route, const std::string& path) {
      if (route.match == Match::Exact) {
        return path == route.path;
      }
      std::size_t n = std::strlen(route.path);
      return path.size() >= n && path.compare(0, n, route.path) == 0;
    }

  }  // namespace

  void Next::operator()(HttpContext& ctx) {
    if (index < route.middleware.count) {
      Middleware current = route.middleware.items[index];
      Next onward{route, conn, index + 1};
      current(ctx, onward);
    }
    else {
      route.handler(ctx);
    }
  }

  void dispatch_http(Connection& conn, const HttpRequest& request) {
    const Route* matched = nullptr;
    bool path_matched = false;
    for (const Route& route : kRoutes) {
      if (!path_matches(route, request.path)) {
        continue;
      }
      path_matched = true;
      if (request.method == route.method) {
        matched = &route;
        break;
      }
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
      param = request.path.substr(std::strlen(matched->path));
    }

    HttpContext ctx(conn, request, std::move(param));
    Next next{*matched, conn, 0};
    next(ctx);  // runs the middleware chain, then the handler

    if (ctx.respond) {
      conn.send_http_simple(ctx.status, reason_phrase(ctx.status), ctx.type,
                            ctx.body, ctx.headers);
    }
  }

}
