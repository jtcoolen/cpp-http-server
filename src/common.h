#pragma once
//
// Shared, dependency-light types.  Nothing in this header touches a socket or
// epoll, which keeps the parser and the worker side easy to audit.
//

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace http {

// ---------------------------------------------------------------------------
// ByteBuf
//
// A contiguous byte buffer with a read cursor.  Consumed bytes are dropped
// lazily: the prefix is only memmove'd away when it accounts for at least half
// of the buffer, so streaming an 8 MiB body through it is O(n) rather than the
// O(n^2) you get from erase(0, n) on every read.
//
// Not thread safe by design: input buffers belong to the event-loop thread.
// ---------------------------------------------------------------------------
class ByteBuf {
public:
  std::size_t size() const noexcept { return data_.size() - start_; }
  bool empty() const noexcept { return size() == 0; }
  const char* data() const noexcept { return data_.data() + start_; }
  std::string_view view() const noexcept { return std::string_view(data(), size()); }

  void append(const char* p, std::size_t n) {
    compact_for_append(n);
    data_.append(p, n);
  }
  void append(std::string_view s) { append(s.data(), s.size()); }
  void append(char c) { compact_for_append(1); data_.push_back(c); }

  // Mark the first n bytes as consumed.  n must be <= size().
  void consume(std::size_t n) {
    start_ += n;
    if (start_ == data_.size()) {
      data_.clear();
      start_ = 0;
    }
  }

  // Remove the consumed prefix now.
  void compact() {
    if (start_ > 0) {
      data_.erase(0, start_);
      start_ = 0;
    }
  }

  // Returns the underlying string with the consumed prefix already dropped, so
  // that callers can plain `out.append(...)` into it.  Any cursor the caller
  // held across this call must be considered invalid afterwards.
  std::string& append_target() {
    compact();
    return data_;
  }

  std::string take_front(std::size_t n) {
    std::string s(data(), n);
    consume(n);
    return s;
  }

  void clear() noexcept {
    data_.clear();
    start_ = 0;
  }

  // Bytes currently held, including the consumed-but-not-yet-dropped prefix.
  std::size_t allocated_bytes() const noexcept { return data_.size(); }

private:
  void compact_for_append(std::size_t incoming) {
    if (start_ == 0)
      return;
    const std::size_t live = data_.size() - start_;
    if (start_ >= live || data_.size() + incoming > data_.capacity())
      compact();
  }

  std::string data_;
  std::size_t start_ = 0;
};

// ---------------------------------------------------------------------------
// HTTP data model
// ---------------------------------------------------------------------------

enum class Method : std::uint8_t {
  Get,
  Head,
  Post,
  Options,
  Unknown, // syntactically valid extension method: 405, not 400
};

struct Header {
  std::string name;
  std::string value;
};

struct Request {
  Method method = Method::Unknown;
  std::string method_name; // exactly as received (used for logs / Allow hints)
  std::string target;      // origin-form path+query, or "*"
  int major = 1;
  int minor = 1;
  std::vector<Header> headers;
  std::string body;
  bool keep_alive = true;     // version + Connection header resolved
  bool expect_100_continue = false;
  std::size_t header_bytes = 0;
  std::size_t total_bytes = 0;
  std::string peer; // textual peer address; diagnostics only, never trusted

  bool is_head() const noexcept { return method == Method::Head; }
  bool is_options() const noexcept { return method == Method::Options; }
  bool is_star_target() const noexcept { return target == "*"; }

  // First match, case-insensitive.  Returns "" when absent.
  std::string_view header(std::string_view name) const noexcept;
  std::vector<std::string> header_all(std::string_view name) const;
};

struct Response {
  int status = 200;
  std::vector<Header> headers;
  std::string body;
  bool keep_alive = true;
  bool close_after = false;    // flush, then close
  bool send_body = true;       // false for HEAD
  bool no_body_framing = false; // true for 204/304: no Content-Length at all
  int req_major = 1;            // version of the request we are answering
  int req_minor = 1;

  void set(std::string_view name, std::string_view value) {
    headers.push_back(Header{std::string(name), std::string(value)});
  }
};

// ---------------------------------------------------------------------------
// Helpers (implemented in http_common.cpp)
// ---------------------------------------------------------------------------

const char* status_reason(int status) noexcept;

// RFC 7230 tchar sequence.
bool valid_token(std::string_view s) noexcept;
// Rejects CR, LF, NUL, other control chars (HTAB allowed) and DEL.
bool valid_header_value(std::string_view s) noexcept;
// Strict: digits only, no sign, no leading zeros, no overflow.
bool parse_content_length(std::string_view value, std::uint64_t& out) noexcept;
std::string trim_ows(std::string_view s);
bool equals_ignore_case(std::string_view a, std::string_view b) noexcept;
std::string to_lower(std::string_view s);
// Splits on ',' and trims OWS around each token.
std::vector<std::string> split_list(std::string_view value);

// Removes CR/LF/NUL so nothing client-controlled can inject a header line.
std::string sanitize_header_value(std::string_view value);

// Cached per second and per thread (only ever called on the event-loop thread
// today; thread_local keeps it correct if that ever changes).
std::string http_date_now();

// Serialises status line + headers + (optionally) body.  Appends to `out`.
void serialize_response(const Response& res, std::string& out);

Response make_error_response(int status, std::string_view detail, bool keep_alive);

} // namespace http
