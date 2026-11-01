#include "http_context.h"

#include "connection.h"
#include "server.h"

namespace dicker {

  Server& HttpContext::server() const {
    return *conn.server_;
  }

  void HttpContext::begin_download(const std::string& session_id,
                                   const std::string& dir_path) {
    respond = false;
    conn.begin_download_response(session_id, dir_path);
  }

}
