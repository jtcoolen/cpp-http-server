#include "handler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace http {

namespace {

constexpr std::string_view kAllow = "GET, HEAD, POST, OPTIONS";
constexpr std::uint64_t kMaxSleepMs = 10000;

void finalize(Response& res, const Request& req) {
  res.req_major = req.major;
  res.req_minor = req.minor;
  res.keep_alive = req.keep_alive;
  if (req.is_head())
    res.send_body = false; // headers (including Content-Length) only
}

Response text_response(const Request& req, int status, std::string body,
                       std::string_view content_type = "text/plain; charset=utf-8") {
  Response res;
  res.status = status;
  res.body = std::move(body);
  res.set("Content-Type", content_type);
  finalize(res, req);
  return res;
}

Response error_response(const Request& req, int status, std::string_view detail) {
  Response res = make_error_response(status, detail, req.keep_alive);
  res.req_major = req.major;
  res.req_minor = req.minor;
  if (req.is_head())
    res.send_body = false;
  return res;
}

Response options_response(const Request& req) {
  Response res;
  res.status = 204;
  res.set("Allow", kAllow);
  res.set("Content-Type", "text/plain; charset=utf-8");
  res.no_body_framing = true; // 204 must not carry Content-Length
  res.send_body = false;
  res.keep_alive = req.keep_alive;
  res.req_major = req.major;
  res.req_minor = req.minor;
  return res;
}

// Accepts "/prefix/123", "/prefix/123?x=y" and "/prefix?n=123".
bool extract_number(std::string_view target, std::string_view prefix, std::uint64_t& out) {
  if (target.size() < prefix.size() || target.substr(0, prefix.size()) != prefix)
    return false;
  const std::string_view rest = target.substr(prefix.size());
  std::size_t i = 0;
  while (i < rest.size() && (rest[i] < '0' || rest[i] > '9'))
    ++i;
  std::size_t begin = i;
  while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9')
    ++i;
  if (i == begin)
    return false;
  std::uint64_t v = 0;
  for (std::size_t k = begin; k < i; ++k) {
    v = v * 10u + static_cast<std::uint64_t>(rest[k] - '0');
    if (v > (1ull << 40)) {
      v = (1ull << 40);
      break;
    }
  }
  out = v;
  return true;
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    const auto uc = static_cast<unsigned char>(c);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      // Anything client-controlled that is not printable ASCII is escaped, so a
      // body/preview can never smuggle control characters into a response.
      if (uc < 0x20 || uc >= 0x7f) {
        static constexpr char kHex[] = "0123456789abcdef";
        out += "\\u00";
        out.push_back(kHex[(uc >> 4) & 0xf]);
        out.push_back(kHex[uc & 0xf]);
      } else {
        out.push_back(c);
      }
    }
  }
  return out;
}

std::string body_preview(const Request& req, std::size_t limit = 256) {
  std::string out;
  out.reserve(32 + req.body.size());
  out += "{\"method\":\"";
  out += json_escape(req.method_name);
  out += "\",\"target\":\"";
  out += json_escape(req.target);
  out += "\",\"version\":\"1.";
  out += (req.minor == 0 ? '0' : '1');
  out += "\",\"headers\":";
  out += std::to_string(req.headers.size());
  out += ",\"body_bytes\":";
  out += std::to_string(req.body.size());
  if (!req.body.empty()) {
    out += ",\"body_preview\":\"";
    out += json_escape(req.body.substr(0, limit));
    out += '"';
  }
  out += "}\n";
  return out;
}

Response handle_get_head(const Request& req, const Config& cfg) {
  const std::string_view t = req.target;
  const std::string_view path = t.substr(0, t.find('?'));

  if (path == "/" || path == "/index.html")
    return text_response(req, 200,
                         "{\"status\":\"ok\",\"server\":\"cpp-http\",\"path\":\"/\"}\n",
                         "application/json");
  if (path == "/health")
    return text_response(req, 200, "ok\n");
  if (path == "/info")
    return text_response(req, 200, body_preview(req), "application/json");
  if (path == "/echo") {
    std::string_view ctype = req.header("Content-Type");
    if (ctype.empty() || !valid_header_value(ctype))
      ctype = "text/plain; charset=utf-8";
    return text_response(req, 200, req.body, ctype);
  }
  if (path == "/boom")
    return error_response(req, 500, "simulated handler failure");
  if (path == "/close") {
    Response res = text_response(req, 200, "closing\n");
    res.close_after = true; // honours "one response, then close"
    res.keep_alive = false;
    return res;
  }

  std::uint64_t n = 0;
  if (extract_number(t, "/big", n)) {
    const auto capped = std::min<std::uint64_t>(n, cfg.max_response_bytes);
    return text_response(req, 200, std::string(static_cast<std::size_t>(capped), 'x'));
  }
  std::uint64_t ms = 0;
  if (extract_number(t, "/slow", ms)) {
    const auto capped_ms = std::min<std::uint64_t>(ms, kMaxSleepMs);
    // Blocking application work - exactly what the pool exists to absorb.
    std::this_thread::sleep_for(std::chrono::milliseconds(capped_ms));
    return text_response(req, 200, "slept " + std::to_string(capped_ms) + "ms\n");
  }

  return error_response(req, 404, "no such resource");
}

Response handle_post(const Request& req, const Config& cfg) {
  const std::string_view t = req.target;
  const std::string_view path = t.substr(0, t.find('?'));

  // Routes whose answer does not depend on the method (the body, if any, has
  // already been framed and consumed by the parser).
  if (path == "/info")
    return text_response(req, 200, body_preview(req), "application/json");
  if (path == "/health")
    return text_response(req, 200, "ok\n");
  if (path == "/")
    return text_response(req, 200, "{\"status\":\"ok\",\"server\":\"cpp-http\",\"path\":\"/\"}\n",
                         "application/json");

  std::uint64_t ms = 0;
  if (extract_number(t, "/slow", ms)) {
    const auto capped_ms = std::min<std::uint64_t>(ms, kMaxSleepMs);
    std::this_thread::sleep_for(std::chrono::milliseconds(capped_ms));
    return text_response(req, 200, "slept " + std::to_string(capped_ms) + "ms\n");
  }
  if (path == "/echo") {
    std::string_view ctype = req.header("Content-Type");
    if (ctype.empty() || !valid_header_value(ctype))
      ctype = "application/octet-stream";
    return text_response(req, 200, req.body, ctype);
  }
  if (path == "/upload") {
    std::string body = "{\"received\":" + std::to_string(req.body.size()) + ",\"sha_hint\":" +
                       std::to_string(req.body.size() % 1000003) + "}\n";
    return text_response(req, 200, std::move(body), "application/json");
  }
  if (path == "/boom")
    return error_response(req, 500, "simulated handler failure");
  (void)cfg;
  return error_response(req, 404, "no such resource");
}

} // namespace

Response handle_request(const Request& req, const Config& cfg) {
  if (req.is_star_target()) {
    if (req.is_options())
      return options_response(req);
    return error_response(req, 400, "'*' target is only valid for OPTIONS");
  }

  switch (req.method) {
  case Method::Get:
  case Method::Head:
    return handle_get_head(req, cfg);
  case Method::Post:
    return handle_post(req, cfg);
  case Method::Options:
    return options_response(req);
  case Method::Unknown:
    break;
  }
  // Syntactically valid method we do not implement: 405 (with Allow), and the
  // request body (if any) has already been framed and consumed by the parser,
  // so the connection stays protocol-correct.
  return error_response(req, 405, "method not allowed");
}

} // namespace http
