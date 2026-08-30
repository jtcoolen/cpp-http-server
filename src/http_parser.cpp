#include "http_parser.h"

#include <algorithm>

namespace http {

namespace {

Method method_from(std::string_view m) noexcept {
  if (m == "GET")
    return Method::Get;
  if (m == "HEAD")
    return Method::Head;
  if (m == "POST")
    return Method::Post;
  if (m == "OPTIONS")
    return Method::Options;
  return Method::Unknown;
}

bool hex_digit(char c, unsigned& out) noexcept {
  if (c >= '0' && c <= '9') {
    out = static_cast<unsigned>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    out = static_cast<unsigned>(c - 'a' + 10);
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    out = static_cast<unsigned>(c - 'A' + 10);
    return true;
  }
  return false;
}

} // namespace

void HttpRequestParser::reset() noexcept {
  state_ = State::kRequestLine;
  cur_ = Request{};
  scan_ = 0;
  consumed_ = 0;
  header_bytes_ = 0;
  body_remaining_ = 0;
  chunk_remaining_ = 0;
  expect_chunk_crlf_ = false;
  chunked_ = false;
  expect_sent_ = false;
  too_large_ = false;
  if (body_.capacity() > 64u * 1024u)
    body_ = std::string(); // do not pin megabytes per idle keep-alive connection
  body_.clear();
}

bool HttpRequestParser::line_ready(const ByteBuf& buf, std::size_t& line_end, bool& bare_lf) noexcept {
  bare_lf = false;
  const char* p = buf.data();
  const std::size_t n = buf.size();
  // Scan for '\n'.  The matching '\r' may sit in a previous read; that is fine
  // because the buffer is contiguous and unconsumed bytes are never dropped.
  for (std::size_t i = scan_ < n ? scan_ : n; i < n; ++i) {
    if (p[i] != '\n')
      continue;
    if (i == 0 || p[i - 1] != '\r') {
      scan_ = i;
      bare_lf = true;
      return false;
    }
    scan_ = i + 1;
    line_end = i - 1;
    return true;
  }
  scan_ = n;
  return false;
}

bool HttpRequestParser::parse_request_line(std::string_view line, Request& out, std::string& err) {
  if (line.empty()) {
    err = "empty request line";
    return false;
  }
  for (char c : line) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc == 0x7f) {
      err = "control character in request line";
      return false;
    }
  }

  // Strict "METHOD SP TARGET SP VERSION": exactly two spaces, no empty field.
  const auto sp1 = line.find(' ');
  if (sp1 == std::string_view::npos) {
    err = "malformed request line";
    return false;
  }
  const auto sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos) {
    err = "malformed request line (missing HTTP version)";
    return false;
  }
  if (line.find(' ', sp2 + 1) != std::string_view::npos) {
    err = "extra whitespace in request line";
    return false;
  }

  const std::string_view m = line.substr(0, sp1);
  const std::string_view t = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string_view v = line.substr(sp2 + 1);
  if (m.empty() || t.empty() || v.empty()) {
    err = "empty request-line field";
    return false;
  }
  if (!valid_token(m)) {
    err = "invalid method";
    return false;
  }
  if (v != "HTTP/1.0" && v != "HTTP/1.1") {
    err = "unsupported HTTP version (only 1.0 and 1.1)";
    return false;
  }

  out.method = method_from(m);
  out.method_name = std::string(m);
  out.major = 1;
  out.minor = (v.back() == '0') ? 0 : 1;

  if (t == "*") {
    if (out.method != Method::Options) {
      err = "'*' request-target is only valid for OPTIONS";
      return false;
    }
  } else {
    if (t.front() != '/') {
      // absolute-form ("http://host/path"), authority-form and anything else
      // we must not proxy.
      err = "only origin-form request targets are accepted";
      return false;
    }
    if (t.size() > 1 && t[1] == '/') {
      // Protocol-relative target ("//host/path") - rejected on purpose: it is
      // the classic SSRF/open-redirect shape and has no meaning origin-form.
      err = "protocol-relative request target is not accepted";
      return false;
    }
    if (t.find('#') != std::string_view::npos) {
      err = "request target must not contain a fragment";
      return false;
    }
    if (t.size() > limits_.max_request_line_bytes) {
      err = "request target too long";
      too_large_ = true;
      return false;
    }
  }
  out.target = std::string(t);
  return true;
}

bool HttpRequestParser::parse_header_line(std::string_view line, Request& out, std::string& err) {
  if (line.front() == ' ' || line.front() == '\t') {
    err = "obs-fold header continuation is not accepted";
    return false;
  }
  const auto colon = line.find(':');
  if (colon == std::string_view::npos) {
    err = "malformed header line (no colon)";
    return false;
  }
  if (colon == 0) {
    err = "empty header name";
    return false;
  }
  const std::string_view name = line.substr(0, colon);
  if (!valid_token(name)) {
    err = "invalid header name";
    return false;
  }
  std::string value = trim_ows(line.substr(colon + 1));
  if (!valid_header_value(value)) {
    err = "invalid characters in header value";
    return false;
  }
  if (out.headers.size() >= limits_.max_header_fields) {
    err = "too many header fields";
    too_large_ = true;
    return false;
  }
  out.headers.push_back(Header{std::string(name), std::move(value)});
  return true;
}

HttpRequestParser::HeadOutcome HttpRequestParser::finish_headers(Request& out, std::string& err) {
  too_large_ = false;

  // --- Connection tokens ----------------------------------------------------
  bool close_requested = false;
  bool ka_requested = false;
  for (const auto& value : out.header_all("Connection")) {
    for (const auto& token : split_list(value)) {
      const std::string t = to_lower(token);
      if (t.empty())
        continue; // tolerate "Connection: keep-alive, "
      if (t == "close")
        close_requested = true;
      else if (t == "keep-alive")
        ka_requested = true;
      else {
        // "Upgrade" and friends: protocol upgrades are not supported, and
        // silently ignoring them would be a security hazard.
        err = "unsupported Connection option";
        return HeadOutcome::kError;
      }
    }
  }
  // HTTP/1.1 keeps connections alive unless asked to close; HTTP/1.0 needs an
  // explicit "Connection: keep-alive".
  out.keep_alive = !close_requested && (out.minor >= 1 ? true : ka_requested);

  // --- Message framing ------------------------------------------------------
  const std::vector<std::string> cl_fields = out.header_all("Content-Length");
  const std::vector<std::string> te_fields = out.header_all("Transfer-Encoding");

  bool use_chunked = false;
  bool have_length = false;
  std::uint64_t length = 0;

  if (!te_fields.empty()) {
    if (out.minor < 1) {
      err = "chunked transfer encoding requires HTTP/1.1";
      return HeadOutcome::kError;
    }
    if (te_fields.size() != 1) {
      err = "multiple Transfer-Encoding headers";
      return HeadOutcome::kError;
    }
    const std::vector<std::string> tokens = split_list(te_fields[0]);
    if (tokens.size() != 1) {
      err = "ambiguous Transfer-Encoding";
      return HeadOutcome::kError;
    }
    if (!equals_ignore_case(tokens[0], "chunked")) {
      err = "unsupported Transfer-Encoding";
      return HeadOutcome::kError;
    }
    use_chunked = true;
  }

  if (!cl_fields.empty()) {
    if (use_chunked) {
      // RFC 7230 leaves the choice to the peer; we refuse instead of guessing.
      err = "Content-Length together with Transfer-Encoding";
      return HeadOutcome::kError;
    }
    if (cl_fields.size() > 1) {
      // Stricter than RFC 7230 on purpose: repeated Content-Length fields are
      // the request-smuggling primitive, so even identical duplicates are out.
      err = "duplicate Content-Length header";
      return HeadOutcome::kError;
    }
    const std::vector<std::string> values = split_list(cl_fields[0]);
    if (values.size() != 1) {
      err = "multiple Content-Length values in one header";
      return HeadOutcome::kError;
    }
    if (!parse_content_length(values[0], length)) {
      err = "malformed Content-Length";
      return HeadOutcome::kError;
    }
    if (length > limits_.max_body_bytes) {
      err = "Content-Length exceeds the body limit";
      too_large_ = true;
      return HeadOutcome::kError;
    }
    have_length = true;
  }

  if (use_chunked) {
    chunked_ = true;
    chunk_remaining_ = 0;
    expect_chunk_crlf_ = false;
    state_ = State::kBodyChunked;
  } else if (have_length && length > 0) {
    body_remaining_ = length;
    state_ = State::kBodyFixed;
  } else {
    return HeadOutcome::kRequestReady; // no body
  }

  // --- Expect: 100-continue (only meaningful when a body is expected) --------
  const std::string_view expect = out.header("Expect");
  if (!expect.empty()) {
    if (!equals_ignore_case(trim_ows(expect), "100-continue")) {
      err = "unsupported Expect";
      return HeadOutcome::kError;
    }
    if (out.minor >= 1 && !expect_sent_) {
      expect_sent_ = true;
      out.expect_100_continue = true;
      return HeadOutcome::kSend100;
    }
  }
  return HeadOutcome::kBodyPending;
}

bool HttpRequestParser::parse_chunk_size(std::string_view line, std::uint64_t& size, std::string& err) {
  // "hex [ chunk-ext ] " - chunk extensions are parsed and discarded.
  const auto semi = line.find(';');
  const std::string_view hex = semi == std::string_view::npos ? line : line.substr(0, semi);
  if (hex.empty() || hex.size() > 16) {
    err = "malformed chunk size";
    return false;
  }
  std::uint64_t v = 0;
  for (char c : hex) {
    unsigned d = 0;
    if (!hex_digit(c, d)) {
      err = "malformed chunk size";
      return false;
    }
    v = v * 16u + d;
    if (v > (1ull << 52)) {
      err = "chunk size out of range";
      too_large_ = true;
      return false;
    }
  }
  size = v;
  return true;
}

ParseStatus HttpRequestParser::parse(ByteBuf& buf, Request& out, std::string& err) {
  err.clear();

  auto emit_request = [&]() {
    out = std::move(cur_);
    out.body = std::move(body_);
    out.header_bytes = header_bytes_;
    out.total_bytes = consumed_;
    reset();
    return ParseStatus::kRequest;
  };

  for (;;) {
    // Hard cap on the request under construction, including whatever is still
    // buffered.  This is what keeps client-driven allocation bounded.
    if (consumed_ + buf.size() > limits_.max_request_bytes) {
      err = "request exceeds the configured limit";
      return ParseStatus::kTooLarge;
    }

    switch (state_) {
    case State::kRequestLine: {
      std::size_t line_end = 0;
      bool bare_lf = false;
      if (!line_ready(buf, line_end, bare_lf)) {
        if (bare_lf) {
          err = "bare LF in request line (strict CRLF required)";
          return ParseStatus::kBadRequest;
        }
        if (buf.size() > limits_.max_request_line_bytes) {
          err = "request line too long";
          return ParseStatus::kTooLarge;
        }
        return ParseStatus::kNeedMore;
      }
      const std::string_view line(buf.data(), line_end);
      if (line.size() > limits_.max_request_line_bytes) {
        err = "request line too long";
        return ParseStatus::kTooLarge;
      }
      const bool ok = parse_request_line(line, cur_, err);
      const std::size_t frame = line_end + 2;
      buf.consume(frame);
      consumed_ += frame;
      scan_ = 0;
      if (!ok)
        return too_large_ ? ParseStatus::kTooLarge : ParseStatus::kBadRequest;
      state_ = State::kHeaders;
      continue;
    }

    case State::kHeaders: {
      std::size_t line_end = 0;
      bool bare_lf = false;
      if (!line_ready(buf, line_end, bare_lf)) {
        if (bare_lf) {
          err = "bare LF in header block (strict CRLF required)";
          return ParseStatus::kBadRequest;
        }
        if (header_bytes_ + buf.size() > limits_.max_header_bytes) {
          err = "header block too large";
          return ParseStatus::kTooLarge;
        }
        return ParseStatus::kNeedMore;
      }
      const std::size_t frame = line_end + 2;
      if (line_end == 0) {
        // End of the header block.
        buf.consume(frame);
        consumed_ += frame;
        header_bytes_ += frame;
        scan_ = 0;
        const HeadOutcome outcome = finish_headers(cur_, err);
        switch (outcome) {
        case HeadOutcome::kError:
          return too_large_ ? ParseStatus::kTooLarge : ParseStatus::kBadRequest;
        case HeadOutcome::kRequestReady:
          return emit_request();
        case HeadOutcome::kSend100:
          return ParseStatus::kExpect100;
        case HeadOutcome::kBodyPending:
          continue;
        }
        continue;
      }
      const std::string_view line(buf.data(), line_end);
      const bool ok = parse_header_line(line, cur_, err);
      buf.consume(frame);
      consumed_ += frame;
      header_bytes_ += frame;
      scan_ = 0;
      if (!ok)
        return too_large_ ? ParseStatus::kTooLarge : ParseStatus::kBadRequest;
      if (header_bytes_ > limits_.max_header_bytes) {
        err = "header block too large";
        return ParseStatus::kTooLarge;
      }
      continue;
    }

    case State::kBodyFixed: {
      const std::size_t take =
          std::min<std::size_t>(static_cast<std::size_t>(body_remaining_), buf.size());
      if (take > 0) {
        body_.append(buf.data(), take);
        buf.consume(take);
        consumed_ += take;
        body_remaining_ -= take;
        scan_ = 0;
      }
      if (body_remaining_ == 0)
        return emit_request();
      return ParseStatus::kNeedMore;
    }

    case State::kBodyChunked: {
      for (;;) {
        if (expect_chunk_crlf_) {
          std::size_t line_end = 0;
          bool bare_lf = false;
          if (!line_ready(buf, line_end, bare_lf)) {
            if (bare_lf) {
              err = "bare LF in chunked stream";
              return ParseStatus::kBadRequest;
            }
            return ParseStatus::kNeedMore;
          }
          if (line_end != 0) {
            err = "chunk data not terminated by CRLF";
            return ParseStatus::kBadRequest;
          }
          buf.consume(2);
          consumed_ += 2;
          scan_ = 0;
          expect_chunk_crlf_ = false;
          continue;
        }

        if (chunk_remaining_ > 0) {
          const std::size_t take =
              std::min<std::size_t>(static_cast<std::size_t>(chunk_remaining_), buf.size());
          if (take == 0)
            return ParseStatus::kNeedMore;
          if (body_.size() + take > limits_.max_body_bytes) {
            err = "chunked body exceeds the body limit";
            return ParseStatus::kTooLarge;
          }
          body_.append(buf.data(), take);
          buf.consume(take);
          consumed_ += take;
          chunk_remaining_ -= take;
          scan_ = 0;
          if (chunk_remaining_ == 0)
            expect_chunk_crlf_ = true;
          continue;
        }

        std::size_t line_end = 0;
        bool bare_lf = false;
        if (!line_ready(buf, line_end, bare_lf)) {
          if (bare_lf) {
            err = "bare LF in chunked stream";
            return ParseStatus::kBadRequest;
          }
          if (buf.size() > limits_.max_request_line_bytes) {
            err = "chunk size line too long";
            return ParseStatus::kTooLarge;
          }
          return ParseStatus::kNeedMore;
        }
        const std::string_view line(buf.data(), line_end);
        std::uint64_t size = 0;
        const bool ok = parse_chunk_size(line, size, err);
        const std::size_t frame = line_end + 2;
        buf.consume(frame);
        consumed_ += frame;
        scan_ = 0;
        if (!ok)
          return too_large_ ? ParseStatus::kTooLarge : ParseStatus::kBadRequest;
        if (size == 0) {
          // Last chunk.  Leave the inner loop so the outer switch re-dispatches
          // into the trailer state (continuing here would read a second
          // chunk-size line, which is the classic "0\r\n\r\n" bug).
          state_ = State::kChunkTrailer;
          break;
        }
        if (body_.size() + size > limits_.max_body_bytes) {
          err = "chunked body exceeds the body limit";
          return ParseStatus::kTooLarge;
        }
        chunk_remaining_ = size;
        continue;
      }
      break; // inner loop only: return to the outer switch (state may have changed)
    }

    case State::kChunkTrailer: {
      for (;;) {
        std::size_t line_end = 0;
        bool bare_lf = false;
        if (!line_ready(buf, line_end, bare_lf)) {
          if (bare_lf) {
            err = "bare LF in trailer block";
            return ParseStatus::kBadRequest;
          }
          if (header_bytes_ + buf.size() > limits_.max_header_bytes) {
            err = "trailer block too large";
            return ParseStatus::kTooLarge;
          }
          return ParseStatus::kNeedMore;
        }
        const std::size_t frame = line_end + 2;
        if (line_end == 0) {
          buf.consume(frame);
          consumed_ += frame;
          scan_ = 0;
          return emit_request();
        }
        const std::string_view line(buf.data(), line_end);
        // Trailers are validated like headers and then ignored (we do not use
        // them for framing).  Accepting them is required by RFC 7230 4.1.4.
        const bool ok = parse_header_line(line, cur_, err);
        buf.consume(frame);
        consumed_ += frame;
        header_bytes_ += frame;
        scan_ = 0;
        if (!ok)
          return too_large_ ? ParseStatus::kTooLarge : ParseStatus::kBadRequest;
        if (header_bytes_ > limits_.max_header_bytes) {
          err = "trailer block too large";
          return ParseStatus::kTooLarge;
        }
        continue;
      }
    }
    } // switch
  }   // for
}

} // namespace http
