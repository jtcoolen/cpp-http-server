#include "common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace http {

namespace {

constexpr std::string_view kAllowList = "GET, HEAD, POST, OPTIONS";

bool is_tchar(char c) noexcept {
  if (c >= 'a' && c <= 'z')
    return true;
  if (c >= 'A' && c <= 'Z')
    return true;
  if (c >= '0' && c <= '9')
    return true;
  switch (c) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

bool is_ows(char c) noexcept { return c == ' ' || c == '\t'; }

} // namespace

const char* status_reason(int status) noexcept {
  switch (status) {
  case 100:
    return "Continue";
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 204:
    return "No Content";
  case 304:
    return "Not Modified";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 408:
    return "Request Timeout";
  case 411:
    return "Length Required";
  case 413:
    return "Payload Too Large";
  case 414:
    return "URI Too Long";
  case 415:
    return "Unsupported Media Type";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 503:
    return "Service Unavailable";
  case 505:
    return "HTTP Version Not Supported";
  default:
    return "Unknown";
  }
}

bool valid_token(std::string_view s) noexcept {
  if (s.empty())
    return false;
  for (char c : s) {
    if (!is_tchar(c))
      return false;
  }
  return true;
}

bool valid_header_value(std::string_view s) noexcept {
  for (char c : s) {
    const auto uc = static_cast<unsigned char>(c);
    if (c == '\t')
      continue;
    if (uc < 0x20 || uc == 0x7f)
      return false; // includes CR and LF: header injection is impossible
    // 0x80..0xFF is obs-text; tolerated on input, never echoed back verbatim.
  }
  return true;
}

bool parse_content_length(std::string_view value, std::uint64_t& out) noexcept {
  if (value.empty() || value.size() > 19)
    return false;
  // Strict: no sign, no whitespace inside, no leading zeros ("0" is fine).
  if (value.size() > 1 && value[0] == '0')
    return false;
  std::uint64_t v = 0;
  for (char c : value) {
    if (c < '0' || c > '9')
      return false;
    v = v * 10u + static_cast<std::uint64_t>(c - '0');
    if (v > (1ull << 52)) // absurdly large: refuse before it can overflow
      return false;
  }
  out = v;
  return true;
}

std::string trim_ows(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && is_ows(s[b]))
    ++b;
  while (e > b && is_ows(s[e - 1]))
    --e;
  return std::string(s.substr(b, e - b));
}

bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::vector<std::string> split_list(std::string_view value) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t comma = value.find(',', pos);
    if (comma == std::string_view::npos) {
      out.push_back(trim_ows(value.substr(pos)));
      break;
    }
    out.push_back(trim_ows(value.substr(pos, comma - pos)));
    pos = comma + 1;
  }
  return out;
}

std::string sanitize_header_value(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc == 0x7f)
      continue; // CR/LF/NUL/control: dropped, never written to the wire
    out.push_back(c);
  }
  return out;
}

std::string http_date_now() {
  const std::time_t now = std::time(nullptr);
  static thread_local std::time_t cached = 0;
  static thread_local std::string text;
  if (now == cached && !text.empty())
    return text;
  cached = now;
  std::tm tm{};
  ::gmtime_r(&now, &tm);
  char buf[64];
  if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0)
    return "Thu, 01 Jan 1970 00:00:00 GMT";
  text.assign(buf);
  return text;
}

std::string_view Request::header(std::string_view name) const noexcept {
  for (const auto& h : headers) {
    if (equals_ignore_case(h.name, name))
      return h.value;
  }
  return {};
}

std::vector<std::string> Request::header_all(std::string_view name) const {
  std::vector<std::string> out;
  for (const auto& h : headers) {
    if (equals_ignore_case(h.name, name))
      out.push_back(h.value);
  }
  return out;
}

void serialize_response(const Response& res, std::string& out) {
  char line[96];
  const int written = std::snprintf(line, sizeof(line), "HTTP/%d.%d %d %s\r\n", res.req_major, res.req_minor,
                                    res.status, status_reason(res.status));
  if (written > 0)
    out.append(line, static_cast<std::size_t>(written));

  out.append("Date: ");
  out.append(http_date_now());
  out.append("\r\n");
  out.append("Server: cpp-http/0.1\r\n");

  bool have_content_length = false;
  bool have_content_type = false;
  for (const auto& h : res.headers) {
    if (equals_ignore_case(h.name, "Content-Length"))
      have_content_length = true;
    if (equals_ignore_case(h.name, "Content-Type"))
      have_content_type = true;
    out.append(sanitize_header_value(h.name));
    out.append(": ");
    out.append(sanitize_header_value(h.value));
    out.append("\r\n");
  }

  // Body framing.  204/304 must not carry Content-Length; everything else gets
  // an explicit length (we never use chunked responses, which keeps responses
  // valid for HTTP/1.0 clients too).  For HEAD the length is the length the
  // GET would have produced, but the body bytes are suppressed.
  if (!res.no_body_framing && !have_content_length) {
    out.append("Content-Length: ");
    out.append(std::to_string(res.body.size()));
    out.append("\r\n");
  }
  if (!res.no_body_framing && !have_content_type) {
    out.append("Content-Type: text/plain; charset=utf-8\r\n");
  }

  if (!res.keep_alive)
    out.append("Connection: close\r\n");
  else if (res.req_major == 1 && res.req_minor == 0)
    out.append("Connection: keep-alive\r\n");

  out.append("\r\n");

  if (res.send_body && !res.body.empty())
    out.append(res.body);
}

Response make_error_response(int status, std::string_view detail, bool keep_alive) {
  Response res;
  res.status = status;
  res.keep_alive = keep_alive;
  res.body = status_reason(status);
  res.body.append(": ");
  res.body.append(detail);
  res.body.push_back('\n');
  if (status == 204 || status == 304) {
    res.no_body_framing = true;
    res.send_body = false;
    res.body.clear();
  }
  switch (status) {
  case 405:
    res.set("Allow", kAllowList);
    break;
  case 503:
    res.set("Retry-After", "1");
    break;
  default:
    break;
  }
  // "Connection: close" is emitted by serialize_response() when keep_alive is
  // false, so it is deliberately not added here (no duplicate headers).
  res.keep_alive = keep_alive;
  return res;
}

} // namespace http
