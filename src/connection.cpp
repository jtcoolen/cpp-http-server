#include "connection.h"

#include "event_loop.h"
#include "handler.h"
#include "thread_pool.h"

#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <utility>

namespace http {

namespace {

std::string format_peer(const struct sockaddr_storage& ss, socklen_t len) {
  char host[NI_MAXHOST] = {0};
  char serv[NI_MAXSERV] = {0};
  const int rc = ::getnameinfo(reinterpret_cast<const struct sockaddr*>(&ss), len, host, sizeof(host),
                               serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
  if (rc != 0)
    return "unknown";
  std::string h(host);
  // A dual-stack listener reports IPv4 peers as ::ffff:a.b.c.d.  Print the
  // familiar form; it is purely cosmetic.
  if (ss.ss_family == AF_INET6 && h.rfind("::ffff:", 0) == 0)
    h = h.substr(7);
  return h + ":" + std::string(serv);
}

} // namespace

Connection::Connection(Fd fd, const struct sockaddr_storage& peer, socklen_t peer_len, const Config& cfg,
                       EventLoop& loop)
    : fd_(std::move(fd)),
      id_(EventLoop::next_connection_id()),
      peer_(format_peer(peer, peer_len)),
      cfg_(cfg),
      loop_(loop),
      parser_(ParserLimits{cfg.max_request_line_bytes, cfg.max_header_bytes, cfg.max_body_bytes,
                           cfg.max_request_bytes, 256}),
      last_activity_(Clock::now()) {}

Connection::~Connection() {
  // The descriptor is closed by fd_ (RAII).  Nothing else may be touched here:
  // we may be running on the event-loop thread during a reap, or later if a
  // worker held the last reference through the completion queue.
}

bool Connection::want_read() const noexcept {
  if (!alive_ || close_after_ || peer_eof_ || in_flight_ || draining_)
    return false;
  // Output backpressure: stop reading while the socket has not caught up.  The
  // client's own receive window then closes, which is the correct place to
  // apply pressure.
  return out_.size() < cfg_.max_output_bytes;
}

std::uint32_t Connection::interest_mask() const noexcept {
  std::uint32_t mask = EPOLLET; // edge-triggered, always
  if (want_read())
    mask |= EPOLLIN;
  if (want_write())
    mask |= EPOLLOUT;
  return mask;
}

bool Connection::finished() const noexcept {
  if (!alive_)
    return true;
  return close_after_ && out_.empty() && !in_flight_;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

void Connection::on_readable() {
  if (!alive_ || !want_read())
    return;

  char buf[16 * 1024];
  for (;;) {
    if (!want_read())
      break; // backpressure: keep the bytes we have, stop taking more
    errno = 0;
    const ssize_t n = ::recv(fd_.get(), buf, sizeof(buf), 0);
    if (n > 0) {
      in_.append(buf, static_cast<std::size_t>(n));
      note_activity();
      if (parser_.bytes_consumed() + in_.size() > cfg_.max_request_bytes) {
        queue_error(413, "request exceeds the configured limit");
        flush();
        return;
      }
      continue;
    }
    if (n == 0) { // EOF: client closed its write side
      peer_eof_ = true;
      break;
    }
    const int e = errno;
    if (e == EINTR)
      continue;
    if (e == EAGAIN || e == EWOULDBLOCK)
      break; // drained - this is the only normal exit for an ET reader
    loop_.log_io("recv failed", peer_, e);
    kill();
    return;
  }

  drain_requests();

  if (peer_eof_ && !in_flight_ && out_.empty())
    close_after_ = true;
}

void Connection::drain_requests() {
  for (;;) {
    if (!alive_ || in_flight_ || close_after_)
      return;

    Request req;
    std::string err;
    const ParseStatus st = parser_.parse(in_, req, err);
    switch (st) {
    case ParseStatus::kNeedMore:
      return;
    case ParseStatus::kExpect100: {
      static constexpr std::string_view kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
      out_.append(kContinue);
      flush();
      continue; // resume body parsing (the body may already be buffered)
    }
    case ParseStatus::kBadRequest:
      loop_.log_bad_request(peer_, err);
      queue_error(400, "malformed request");
      flush();
      return;
    case ParseStatus::kTooLarge:
      loop_.log_bad_request(peer_, err);
      queue_error(413, "request too large");
      flush();
      return;
    case ParseStatus::kRequest:
      break;
    }

    req_major_ = req.major;
    req_minor_ = req.minor;
    last_head_ = req.is_head();
    req.peer = peer_; // set after parse(): parse() overwrites the Request
    in_flight_ = true;
    inflight_seq_ = next_seq_++;
    ++served_;
    loop_.note_request_received();
    submit(std::move(req));
    return; // one in-flight request per connection: pipelining stays serialised
  }
}

void Connection::submit(Request&& req) {
  const std::uint64_t cid = id_;
  const std::uint64_t seq = inflight_seq_;
  EventLoop* loop = &loop_;
  const Config* cfg = &cfg_;
  std::weak_ptr<Connection> self = weak_from_this(); // resolved only on the loop thread

  const bool queued = loop_.pool().try_submit(
      [loop, self, cid, seq, cfg, req = std::move(req)]() mutable {
        Response res;
        try {
          res = handle_request(req, *cfg);
        } catch (const std::exception&) {
          res = make_error_response(500, "handler failed", req.keep_alive);
        } catch (...) {
          res = make_error_response(500, "handler failed", req.keep_alive);
        }
        res.req_major = req.major;
        res.req_minor = req.minor;
        if (req.is_head())
          res.send_body = false; // Content-Length yes, body no
        if (!req.keep_alive)
          res.keep_alive = false; // the client asked to close: honour it
        loop->post_completion(Completion{self, cid, seq, std::move(res)});
      });

  if (!queued) {
    // Overload.  Answer 503 here, on the event loop, and close: we never wait
    // for queue space and never block the loop.
    in_flight_ = false;
    loop_.note_overload_rejection();
    queue_error(503, "server is at capacity");
  }
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

void Connection::on_worker_result(Response&& res, std::uint64_t seq) {
  if (!alive_)
    return; // gone (should be impossible: the loop reaps these first)
  if (!in_flight_ || seq != inflight_seq_)
    return; // stale or duplicated result: ignore rather than double-respond
  in_flight_ = false;
  queue_response(std::move(res));
  flush();
}

void Connection::queue_response(Response&& res) {
  if (last_head_)
    res.send_body = false; // correct Content-Length, zero body bytes
  serialize_response(res, out_.append_target());
  if (!res.keep_alive || res.close_after)
    close_after_ = true;
}

void Connection::queue_error(int status, std::string_view detail) {
  // Loop-generated errors always end the connection: after a framing violation,
  // a timeout or an overload there is no way to resynchronise the stream, and
  // guessing would be a request-smuggling hazard.
  Response res = make_error_response(status, detail, /*keep_alive=*/false);
  res.req_major = req_major_;
  res.req_minor = req_minor_;
  queue_response(std::move(res));
  close_after_ = true;
}

void Connection::on_writable() {
  // The socket accepted everything we had queued; after that, two things may
  // have changed: the connection may be finished (close_after_ and empty
  // buffer), or want_read() may have become true again now that the output
  // buffer dropped below the backpressure threshold.  The event loop notices
  // both by re-reading interest_mask(), and re-scans EPOLLIN transitions.
  flush();
}

void Connection::flush() noexcept {
  while (!out_.empty() && alive_) {
    errno = 0;
    const ssize_t n = ::send(fd_.get(), out_.data(), out_.size(), MSG_NOSIGNAL);
    if (n > 0) {
      out_.consume(static_cast<std::size_t>(n));
      note_activity();
      continue; // keep going until EAGAIN: this is an edge-triggered world
    }
    const int e = errno;
    if (e == EINTR)
      continue;
    if (e == EAGAIN || e == EWOULDBLOCK)
      return; // EPOLLOUT will bring us back (interest_mask() sets it)
    // EPIPE / ECONNRESET: the client hung up mid-response.  Expected under
    // load; SIGPIPE is ignored, so this is just a closed connection.
    loop_.log_io("send failed", peer_, e);
    kill();
    return;
  }
}

// ---------------------------------------------------------------------------
// Errors, timeouts, shutdown
// ---------------------------------------------------------------------------

void Connection::on_hangup(std::uint32_t events) {
  if ((events & EPOLLERR) != 0) {
    kill();
    return;
  }
  // EPOLLHUP: the peer is gone.  Push out whatever is queued on a best-effort
  // basis (writes simply fail if the socket is really dead) and then close.
  peer_eof_ = true;
  close_after_ = true;
  flush();
}

void Connection::on_tick(Clock::time_point now) {
  if (!alive_)
    return;

  if (draining_ && !in_flight_ && out_.empty()) {
    close_after_ = true;
    return;
  }

  if (cfg_.idle_timeout_sec <= 0.0)
    return;

  const auto idle =
      std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(cfg_.idle_timeout_sec));
  if (now - last_activity_ <= idle)
    return;

  if (in_flight_)
    return; // slow application work is not an idle connection (see README)

  if (!out_.empty()) {
    // We have bytes stuck for longer than the idle timeout: the client is not
    // reading (slow loris shape).  Stop paying for it.
    loop_.log_io("write stall, closing", peer_, 0);
    kill();
    return;
  }

  // Idle while waiting for a request: 408, then close.
  loop_.note_timeout();
  queue_error(408, "no request within the idle timeout");
  flush();
}

void Connection::kill() noexcept {
  alive_ = false;
  in_flight_ = false; // a late worker result is dropped (alive_ && seq checks)
  close_after_ = true;
  in_.clear();
  out_.clear();
}

} // namespace http
