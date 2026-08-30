#pragma once
//
// Incremental HTTP/1.x request parser.
//
// Design rules:
//   * It never blocks and never waits for data: every call consumes what is
//     available and returns kNeedMore when it needs more.
//   * It never reads past buf.size() and never copies more than it consumes.
//   * Every limit is enforced *while* parsing, never after a request has been
//     fully buffered, so a client cannot make us allocate without bound.
//   * On kBadRequest / kTooLarge the byte stream is no longer resynchronisable;
//     the caller must answer and close the connection.
//

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace http {

struct ParserLimits {
  std::size_t max_request_line_bytes = 8 * 1024;
  std::size_t max_header_bytes = 64 * 1024;
  std::size_t max_body_bytes = 8u * 1024u * 1024u;
  std::size_t max_request_bytes = 9u * 1024u * 1024u;
  std::size_t max_header_fields = 256;
};

enum class ParseStatus : std::uint8_t {
  kNeedMore,  // valid so far, incomplete
  kRequest,   // complete request in `out`
  kExpect100, // headers complete, body pending: caller must send "100 Continue"
              // and then call parse() again
  kBadRequest,
  kTooLarge,
};

class HttpRequestParser {
public:
  explicit HttpRequestParser(ParserLimits limits) : limits_(limits) {}

  ParseStatus parse(ByteBuf& buf, Request& out, std::string& err);

  // Forget the request under construction (called when a connection closes or
  // after a request has been handed over).
  void reset() noexcept;

  // Bytes consumed so far for the request under construction.  The total size
  // of that request is bytes_consumed() + whatever is still buffered.
  std::size_t bytes_consumed() const noexcept { return consumed_; }
  bool in_progress() const noexcept { return state_ != State::kRequestLine || consumed_ > 0; }
  const ParserLimits& limits() const noexcept { return limits_; }

private:
  enum class State : std::uint8_t {
    kRequestLine,
    kHeaders,
    kBodyFixed,
    kBodyChunked,
    kChunkTrailer,
  };

  // Looks for the next CRLF, resuming at scan_.  Returns true when a complete
  // line is available and sets line_end to its length excluding the CRLF.
  // bare_lf reports a LF that is not preceded by CR (strict CRLF violation).
  bool line_ready(const ByteBuf& buf, std::size_t& line_end, bool& bare_lf) noexcept;

  bool parse_request_line(std::string_view line, Request& out, std::string& err);
  bool parse_header_line(std::string_view line, Request& out, std::string& err);
  // Resolves framing + keep-alive once the header block is complete.
  enum class HeadOutcome : std::uint8_t {
    kBodyPending,  // body framing set up, keep parsing
    kRequestReady, // no body: the request is complete
    kSend100,      // body pending, but "100 Continue" must be sent first
    kError,        // err set; too_large_ distinguishes 413 from 400
  };
  HeadOutcome finish_headers(Request& out, std::string& err);
  bool parse_chunk_size(std::string_view line, std::uint64_t& size, std::string& err);

  ParserLimits limits_;
  State state_ = State::kRequestLine;
  Request cur_;

  std::size_t scan_ = 0;        // CRLF search cursor (view-relative)
  std::size_t consumed_ = 0;    // bytes consumed for the current request
  std::size_t header_bytes_ = 0;
  std::uint64_t body_remaining_ = 0;
  std::uint64_t chunk_remaining_ = 0;
  bool expect_chunk_crlf_ = false; // chunk data done, its CRLF still pending
  bool chunked_ = false;
  bool expect_sent_ = false;
  bool too_large_ = false; // set by finish_headers to distinguish 413 from 400
  std::string body_;
};

} // namespace http
