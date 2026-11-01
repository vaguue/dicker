#include "connection.h"
#include "http_context.h"
#include "middleware.h"

namespace dicker {
  namespace mw {

    void require_auth(HttpContext& ctx, Next& next) {
      if (!ctx.conn.authorized(ctx.req)) {
        ctx.status = 401;
        ctx.type = "text/plain; charset=utf-8";
        ctx.body = "unauthorized\n";
        ctx.set("WWW-Authenticate", "Bearer");
        return;  // short-circuit: do not call next()
      }
      next(ctx);
    }

  }  // namespace mw
}
