#pragma once
//
// One client socket.
//
// Threading rule, and the reason there are no locks in this class:
//   every Connection member is touched by the event-loop thread ONLY.  Worker
//   threads receive an immutable copy of the Request and hand the Response back
//   through EventLoop::post_completion(); the response is applied to the
//   connection by the event-loop thread.  Results that come back for a
//   connection that has already been destroyed are dropped (weak_ptr), which is
//   what prevents the classic "worker finishes after the client hung up"
//   use-after-free.
//
// Backpressure (see on_readable / want_read):
//   * input is capped by cfg.max_request_bytes (checked after every recv),
//   * at most ONE request per connection is in flight at a time, so a client
//     that pipelines 1000 requests gets throttled by its own TCP window instead
//     of by our memory,
//   * when the output buffer reaches cfg.max_output_bytes we drop EPOLLIN and
//     stop reading - read-stoppage is the flow-control signal - and resume when
//     the socket drains.
//

#include "common.h"
#include "config.h"
#include "fd.h"
#include "http_parser.h"

#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace http {

class EventLoop;

// Posted by a worker thread, consumed by the event-loop thread.
struct Completion {
  std::weak_ptr<class Connection> conn;
  std::uint64_t conn_id = 0;
  std::uint64_t seq = 0;
  Response response;
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
  using Clock = std::chrono::steady_clock;

  Connection(Fd fd, const struct sockaddr_storage& peer, socklen_t peer_len, const Config& cfg,
             EventLoop& loop);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) = delete;
  Connection& operator=(Connection&&) = delete;

  // --- event-loop thread only ---------------------------------------------
  void on_readable();
  void on_writable();
  void on_hangup(std::uint32_t events);
  void on_tick(Clock::time_point now);
  void on_worker_result(Response&& res, std::uint64_t seq);
  // Graceful shutdown: stop accepting new requests on this connection, finish
  // whatever is in flight, flush, then close.  An idle connection closes right
  // away; a busy one closes as soon as its response has been written.
  void begin_draining() noexcept {
    draining_ = true;
    if (!in_flight_ && out_.empty())
      close_after_ = true;
  }

  int fd() const noexcept { return fd_.get(); }
  std::uint64_t id() const noexcept { return id_; }
  const std::string& peer() const noexcept { return peer_; }

  bool want_read() const noexcept;
  bool want_write() const noexcept { return alive_ && !out_.empty(); }
  std::uint32_t interest_mask() const noexcept;
  std::uint32_t applied_mask() const noexcept { return applied_mask_; }
  void set_applied_mask(std::uint32_t m) noexcept { applied_mask_ = m; }

  bool finished() const noexcept;
  bool alive() const noexcept { return alive_; }
  bool in_flight() const noexcept { return in_flight_; }
  void kill() noexcept;

  // --- diagnostics ---------------------------------------------------------
  std::uint64_t requests_served() const noexcept { return served_; }
  std::size_t output_pending() const noexcept { return out_.size(); }
  std::size_t input_pending() const noexcept { return in_.size(); }

private:
  void drain_requests();
  void submit(Request&& req);
  void flush() noexcept;
  void queue_response(Response&& res);
  void queue_error(int status, std::string_view detail);
  void note_activity() { last_activity_ = Clock::now(); }

  Fd fd_;
  std::uint64_t id_ = 0;
  std::string peer_;
  const Config& cfg_;
  EventLoop& loop_;
  HttpRequestParser parser_;

  ByteBuf in_;
  ByteBuf out_;

  bool alive_ = true;
  bool close_after_ = false; // flush what is queued, then close
  bool draining_ = false;    // graceful shutdown: finish and go away
  bool peer_eof_ = false;    // recv() returned 0
  bool in_flight_ = false;   // exactly one outstanding request per connection
  std::uint64_t inflight_seq_ = 0;
  std::uint64_t next_seq_ = 1;
  std::uint64_t served_ = 0;
  int req_major_ = 1;
  int req_minor_ = 1;
  bool last_head_ = false; // HEAD never produces a body, not even for errors
  std::uint32_t applied_mask_ = 0;
  Clock::time_point last_activity_;
};

} // namespace http
