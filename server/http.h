#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace dicker {

  struct HttpRequest {
    std::string method;   // e.g. "GET"
    std::string target;   // raw request target, e.g. "/uploads?x=1"
    std::string path;     // target with any query string removed
    std::string version;  // e.g. "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers;  // names lowercased

    // Returns the header value for `lower_name`, or nullptr if absent.
    const std::string* header(const std::string& lower_name) const;
  };

  // Index just past the end of the header block ("\r\n\r\n"), or 0 if the block
  // is not yet complete within [data, data+len).
  std::size_t http_header_end(const std::uint8_t* data, std::size_t len);

  // Parse the request line + headers found in [data, data+len). Returns false on
  // a malformed request line.
  bool parse_http_request(const std::uint8_t* data, std::size_t len, HttpRequest& out);

  // Build a response header block. content_length < 0 omits Content-Length.
  std::string http_response_headers(
      int status, const char* reason, const std::string& content_type,
      long long content_length,
      const std::vector<std::pair<std::string, std::string>>& extra = {});

}
