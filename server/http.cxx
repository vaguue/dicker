#include "http.h"

#include <cctype>
#include <cstring>

namespace dicker {

  const std::string* HttpRequest::header(const std::string& lower_name) const {
    for (const auto& h : headers) {
      if (h.first == lower_name) {
        return &h.second;
      }
    }
    return nullptr;
  }

  std::size_t http_header_end(const std::uint8_t* data, std::size_t len) {
    if (len < 4) {
      return 0;
    }
    for (std::size_t i = 0; i + 3 < len; ++i) {
      if (data[i] == '\r' && data[i + 1] == '\n' &&
          data[i + 2] == '\r' && data[i + 3] == '\n') {
        return i + 4;
      }
    }
    return 0;
  }

  namespace {

    std::string trim(const std::string& s) {
      std::size_t a = 0;
      std::size_t b = s.size();
      while (a < b && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
      }
      while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
        --b;
      }
      return s.substr(a, b - a);
    }

    std::string lower(const std::string& s) {
      std::string out = s;
      for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return out;
    }

  }  // namespace

  bool parse_http_request(const std::uint8_t* data, std::size_t len, HttpRequest& out) {
    std::string text(reinterpret_cast<const char*>(data), len);

    std::size_t line_end = text.find("\r\n");
    if (line_end == std::string::npos) {
      return false;
    }
    std::string request_line = text.substr(0, line_end);

    std::size_t sp1 = request_line.find(' ');
    if (sp1 == std::string::npos) {
      return false;
    }
    std::size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
      return false;
    }
    out.method = request_line.substr(0, sp1);
    out.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    out.version = request_line.substr(sp2 + 1);
    if (out.method.empty() || out.target.empty()) {
      return false;
    }

    std::size_t query = out.target.find('?');
    out.path = (query == std::string::npos) ? out.target : out.target.substr(0, query);

    std::size_t pos = line_end + 2;
    while (pos < text.size()) {
      std::size_t next = text.find("\r\n", pos);
      if (next == std::string::npos) {
        next = text.size();
      }
      if (next == pos) {
        break;  // blank line -> end of headers
      }
      std::string header_line = text.substr(pos, next - pos);
      std::size_t colon = header_line.find(':');
      if (colon != std::string::npos) {
        std::string name = lower(trim(header_line.substr(0, colon)));
        std::string value = trim(header_line.substr(colon + 1));
        out.headers.emplace_back(std::move(name), std::move(value));
      }
      pos = next + 2;
    }
    return true;
  }

  std::string http_response_headers(
      int status, const char* reason, const std::string& content_type,
      long long content_length,
      const std::vector<std::pair<std::string, std::string>>& extra) {
    std::string out;
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out.push_back(' ');
    out += reason;
    out += "\r\n";
    if (!content_type.empty()) {
      out += "Content-Type: ";
      out += content_type;
      out += "\r\n";
    }
    if (content_length >= 0) {
      out += "Content-Length: ";
      out += std::to_string(content_length);
      out += "\r\n";
    }
    out += "Server: dicker\r\n";
    for (const auto& h : extra) {
      out += h.first;
      out += ": ";
      out += h.second;
      out += "\r\n";
    }
    out += "\r\n";
    return out;
  }

}
