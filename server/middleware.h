#pragma once

#include <cstddef>
#include <vector>

namespace dicker {

  struct HttpContext;
  struct Connection;
  struct Route;

  struct Next;

  // Koa-style middleware. It does its pre-work, calls next(ctx) to run the rest
  // of the chain (which ends in the route handler), then its post-work on the
  // way back out; or it returns without calling next to short-circuit the
  // request, having set ctx's response.
  using Middleware = void (*)(HttpContext& ctx, Next& next);

  // The continuation handed to each middleware: invoking it runs the next
  // middleware in the route's chain, or the handler once the chain is exhausted.
  struct Next {
    const Route& route;
    Connection& conn;
    std::size_t index;

    void operator()(HttpContext& ctx);
  };

  // A middleware chain: runs in order before the route handler; {} for none.
  using Chain = std::vector<Middleware>;

  namespace mw {
    // Rejects requests without a valid bearer token (see dicker-dbtool).
    void require_auth(HttpContext& ctx, Next& next);
  }

}
