#include <string>
#include "http_context.h"

namespace dicker {
  namespace controllers {

    void root(HttpContext& ctx) {
      static const std::string body =
          "<!doctype html>\n<html><head><meta charset=\"utf-8\">"
          "<title>dicker</title></head>"
          "<body><h1>dicker</h1><p>hello world</p></body></html>\n";
      ctx.type = "text/html; charset=utf-8";
      ctx.body = body;
    }

  }  // namespace controllers
}
