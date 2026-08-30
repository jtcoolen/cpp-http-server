#pragma once
//
// Runtime configuration.  Immutable after start(): the event loop and every
// worker thread hold a const reference / pointer to the same object, which is
// safe precisely because nobody writes it after main() finishes parsing argv.
//

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace http {

struct Config {
  std::string bind = "::"; // "::" dual-stack, "0.0.0.0" v4-only, "::1"/"127.0.0.1" host-only
  std::uint16_t port = 8080;
  std::string port_file; // written with the real port when port == 0 (tests)

  int backlog = 1024;

  std::size_t workers = 4;
  std::size_t max_queue = 1024;      // bounded worker job queue; full -> 503
  std::size_t max_connections = 4096; // listener-level admission control

  // Request limits (all enforced while parsing, never after buffering a whole
  // request).
  std::size_t max_request_line_bytes = 8 * 1024;
  std::size_t max_header_bytes = 64 * 1024;
  std::size_t max_body_bytes = 8u * 1024u * 1024u;
  std::size_t max_request_bytes = 9u * 1024u * 1024u; // line + headers + body

  // Output buffer cap per connection.  This is the backpressure trigger: when
  // the socket will not take bytes fast enough we stop reading from it.
  std::size_t max_output_bytes = 16u * 1024u * 1024u;
  std::size_t max_response_bytes = 8u * 1024u * 1024u; // handler-generated bodies

  double idle_timeout_sec = 15.0;
  double shutdown_timeout_sec = 5.0;
  int tick_ms = 200; // timeout / shutdown scan granularity

  bool verbose = false;

  std::string describe() const;
};

// Returns false with a message in `err` on bad input (also used for -h, which
// sets err to the usage text and `wants_exit_success`).
bool parse_args(int argc, char** argv, Config& cfg, std::string& err, bool& wants_exit_success);

} // namespace http
