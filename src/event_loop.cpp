#include "event_loop.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace http {

std::atomic<std::uint64_t> EventLoop::next_conn_id_{0};

namespace {

constexpr int kMaxEvents = 1024;
constexpr int kAcceptBatch = 256;

const char* errno_name(int e) {
  const char* s = strerror(e);
  return s ? s : "?";
}

} // namespace

EventLoop::EventLoop(Config cfg) : cfg_(std::move(cfg)), pool_(cfg_.workers, cfg_.max_queue) {}

EventLoop::~EventLoop() {
  // Destroy the connections before the pool is torn down?  No: order matters
  // here.  ~EventLoop runs after run() has already stopped and joined the
  // workers, so nothing can be posted anymore.  Closing the sockets is then
  // just RAII.
  pool_.stop();
  conns_.clear();
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

void EventLoop::log_io(std::string_view what, std::string_view peer, int err) noexcept {
  if (err == 0) {
    std::fprintf(stderr, "[io] %.*s (%.*s)\n", static_cast<int>(what.size()), what.data(),
                 static_cast<int>(peer.size()), peer.data());
    return;
  }
  std::fprintf(stderr, "[io] %.*s (%.*s): %s\n", static_cast<int>(what.size()), what.data(),
               static_cast<int>(peer.size()), peer.data(), errno_name(err));
}

void EventLoop::log_bad_request(std::string_view peer, std::string_view why) noexcept {
  if (!cfg_.verbose)
    return;
  std::fprintf(stderr, "[400] %.*s: %.*s\n", static_cast<int>(peer.size()), peer.data(),
               static_cast<int>(why.size()), why.data());
}

void EventLoop::log_note(std::string_view what) noexcept {
  std::fprintf(stderr, "[server] %.*s\n", static_cast<int>(what.size()), what.data());
  std::fflush(stderr);
}

void EventLoop::log_stats() noexcept {
  std::fprintf(stderr,
               "[server] done: accepted=%llu requests=%llu timeouts=%llu overload_503=%llu "
               "rejected=%llu live=%zu\n",
               static_cast<unsigned long long>(accepted_),
               static_cast<unsigned long long>(requests_received_),
               static_cast<unsigned long long>(timeouts_),
               static_cast<unsigned long long>(overload_rejections_),
               static_cast<unsigned long long>(rejected_connections_), conns_.size());
  std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

bool EventLoop::setup_wake() {
  const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    log_io("eventfd", "startup", errno);
    return false;
  }
  wake_fd_ = Fd(fd);
  struct epoll_event ev {};
  ev.events = EPOLLIN; // a counter, not a stream: level-triggered is correct here
  ev.data.fd = fd;
  if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
    log_io("epoll_ctl ADD wake", "startup", errno);
    return false;
  }
  return true;
}

bool EventLoop::setup_signals() {
  // SIGINT/SIGTERM must already be blocked in this process (main() does that
  // before starting any thread, so every worker inherits the mask).
  sigset_t mask;
  ::sigemptyset(&mask);
  ::sigaddset(&mask, SIGINT);
  ::sigaddset(&mask, SIGTERM);
  const int fd = ::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  if (fd < 0) {
    log_io("signalfd", "startup", errno);
    return false;
  }
  sig_fd_ = Fd(fd);
  struct epoll_event ev {};
  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = fd;
  if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
    log_io("epoll_ctl ADD signal", "startup", errno);
    return false;
  }
  return true;
}

bool EventLoop::setup_listener() {
  const bool want_v6 = cfg_.bind.find(':') != std::string::npos;
  const int family = want_v6 ? AF_INET6 : AF_INET;

  const int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    log_io("socket", cfg_.bind, errno);
    return false;
  }

  int one = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
    log_io("setsockopt SO_REUSEADDR", cfg_.bind, errno);
  if (want_v6) {
    // Dual stack by default: one socket serves IPv6 and IPv4 (v4-mapped).
    int off = 0;
    if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) != 0)
      log_io("setsockopt IPV6_V6ONLY (staying dual-stack)", cfg_.bind, errno);
  }

  struct sockaddr_storage addr {};
  socklen_t addr_len = 0;
  if (want_v6) {
    auto* a6 = reinterpret_cast<struct sockaddr_in6*>(&addr);
    a6->sin6_family = AF_INET6;
    a6->sin6_port = htons(cfg_.port);
    if (::inet_pton(AF_INET6, cfg_.bind.c_str(), &a6->sin6_addr) != 1) {
      log_note("invalid IPv6 bind address");
      ::close(fd);
      return false;
    }
    addr_len = sizeof(struct sockaddr_in6);
  } else {
    auto* a4 = reinterpret_cast<struct sockaddr_in*>(&addr);
    a4->sin_family = AF_INET;
    a4->sin_port = htons(cfg_.port);
    if (::inet_pton(AF_INET, cfg_.bind.c_str(), &a4->sin_addr) != 1) {
      log_note("invalid IPv4 bind address (use dotted-quad)");
      ::close(fd);
      return false;
    }
    addr_len = sizeof(struct sockaddr_in);
  }

  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), addr_len) != 0) {
    log_io("bind", cfg_.bind, errno);
    ::close(fd);
    return false;
  }
  if (::listen(fd, cfg_.backlog) != 0) {
    log_io("listen", cfg_.bind, errno);
    ::close(fd);
    return false;
  }

  struct sockaddr_storage bound {};
  socklen_t bound_len = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &bound_len) == 0) {
    if (bound.ss_family == AF_INET6)
      bound_port_ = ntohs(reinterpret_cast<struct sockaddr_in6*>(&bound)->sin6_port);
    else
      bound_port_ = ntohs(reinterpret_cast<struct sockaddr_in*>(&bound)->sin_port);
  }

  struct epoll_event ev {};
  ev.events = EPOLLIN | EPOLLET; // drained to EAGAIN in accept_batch()
  ev.data.fd = fd;
  if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
    log_io("epoll_ctl ADD listener", cfg_.bind, errno);
    ::close(fd);
    return false;
  }

  listen_fd_ = Fd(fd);
  if (!cfg_.port_file.empty())
    write_port_file(bound_port_);

  char line[256];
  std::snprintf(line, sizeof(line), "listening on %s:%u (%s, dual-stack %s) %s", cfg_.bind.c_str(),
                static_cast<unsigned>(bound_port_), want_v6 ? "IPv6" : "IPv4",
                want_v6 ? "yes" : "n/a", cfg_.describe().c_str());
  log_note(line);
  return true;
}

void EventLoop::write_port_file(std::uint16_t port) {
  const std::string tmp = cfg_.port_file + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    log_io("open port file", cfg_.port_file, errno);
    return;
  }
  const std::string text = std::to_string(port) + "\n";
  const ssize_t rc = ::write(fd, text.data(), text.size());
  ::fsync(fd);
  ::close(fd);
  if (rc != static_cast<ssize_t>(text.size())) {
    log_io("write port file", cfg_.port_file, errno);
    return;
  }
  if (::rename(tmp.c_str(), cfg_.port_file.c_str()) != 0)
    log_io("rename port file", cfg_.port_file, errno);
}

// ---------------------------------------------------------------------------
// cross-thread plumbing
// ---------------------------------------------------------------------------

void EventLoop::wake() noexcept {
  const std::uint64_t one = 1;
  for (;;) {
    const ssize_t n = ::write(wake_fd_.get(), &one, sizeof(one));
    if (n == static_cast<ssize_t>(sizeof(one)))
      return;
    const int e = errno;
    if (e == EINTR)
      continue;
    return; // EAGAIN: the counter is already non-zero, a wakeup is pending
  }
}

void EventLoop::post_completion(Completion&& completion) {
  {
    std::lock_guard<std::mutex> lock(comp_mu_);
    completions_.push_back(std::move(completion));
  }
  wake(); // outside the lock: a syscall while holding a mutex is how you get
          // priority-inversion stalls on the loop
}

void EventLoop::drain_wake() {
  for (;;) {
    std::uint64_t counter = 0;
    const ssize_t n = ::read(wake_fd_.get(), &counter, sizeof(counter));
    if (n == static_cast<ssize_t>(sizeof(counter)))
      continue;
    const int e = errno;
    if (e == EINTR)
      continue;
    return; // EAGAIN: drained
  }
}

void EventLoop::drain_completions() {
  std::deque<Completion> batch;
  {
    std::lock_guard<std::mutex> lock(comp_mu_);
    batch.swap(completions_);
  }
  for (auto& c : batch) {
    // The weak_ptr is resolved here, on the event-loop thread, and nowhere
    // else.  If the client disconnected while the worker was busy the lock()
    // returns null and the result is dropped: that is the entire defence
    // against "worker finishes after the client went away".
    std::shared_ptr<Connection> conn = c.conn.lock();
    if (!conn)
      continue;
    if (conn->id() != c.conn_id)
      continue; // defensive: never respond on someone else's fd
    conn->on_worker_result(std::move(c.response), c.seq);
    if (!conn->alive() || conn->finished()) {
      reap_.push_back(conn->fd());
      continue;
    }
    set_interest(conn);
  }
}

std::size_t EventLoop::completions_pending() {
  std::lock_guard<std::mutex> lock(comp_mu_);
  return completions_.size();
}

// ---------------------------------------------------------------------------
// accept
// ---------------------------------------------------------------------------

void EventLoop::accept_batch() {
  for (int i = 0; i < kAcceptBatch; ++i) {
    struct sockaddr_storage peer {};
    socklen_t peer_len = sizeof(peer);
    const int fd = ::accept4(listen_fd_.get(), reinterpret_cast<struct sockaddr*>(&peer), &peer_len,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      const int e = errno;
      if (e == EINTR)
        continue;
      if (e == EAGAIN || e == EWOULDBLOCK)
        return; // accept queue drained (edge-triggered listener)
      if (e == EMFILE || e == ENFILE) {
        // Descriptor exhaustion is transient.  Do not spin: retry from the tick
        // timer, which also keeps the edge-triggered listener from going quiet.
        if (!accept_retry_)
          log_io("accept: out of file descriptors, deferring", "listener", e);
        accept_retry_ = true;
        return;
      }
      if (e == ECONNABORTED)
        continue; // client went away during the handshake
      log_io("accept4", "listener", e);
      return;
    }

    int one = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
      log_io("setsockopt TCP_NODELAY", "accepted", errno);
    if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) != 0)
      log_io("setsockopt SO_KEEPALIVE", "accepted", errno);

    if (draining_) {
      reject_connection(fd, "server is shutting down");
      continue;
    }
    if (conns_.size() >= cfg_.max_connections) {
      reject_connection(fd, "connection limit reached");
      continue;
    }

    auto conn = std::make_shared<Connection>(Fd(fd), peer, peer_len, cfg_, *this);
    struct epoll_event ev {};
    ev.events = conn->interest_mask();
    ev.data.fd = conn->fd();
    if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, conn->fd(), &ev) != 0) {
      log_io("epoll_ctl ADD connection", conn->peer(), errno);
      continue; // conn goes out of scope here and closes the fd
    }
    conn->set_applied_mask(ev.events);
    conns_.emplace(conn->fd(), std::move(conn));
    ++accepted_;
    if (cfg_.verbose)
      log_note("accepted " + std::to_string(conns_.size()) + " live");
  }
  // Hit the batch cap: there may be more pending connections, and an
  // edge-triggered listener will not tell us again.  Retry on the next tick.
  accept_retry_ = true;
}

void EventLoop::reject_connection(int fd, std::string_view reason) {
  // Admission control happens before a Connection object exists, so the 503 is
  // written directly.  The buffer is tiny and the socket brand new, so a single
  // non-blocking send almost always takes it all; if it does not, we close and
  // let the client retry.  Blocking here is not an option.
  Response res = make_error_response(503, reason, false);
  std::string out;
  serialize_response(res, out);

  std::size_t sent = 0;
  for (int tries = 0; tries < 8 && sent < out.size(); ++tries) {
    const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
      continue;
    }
    const int e = errno;
    if (e == EINTR)
      continue;
    break; // EAGAIN or a real error: the client will not see it
  }
  ::close(fd);
  ++rejected_connections_;
}

// ---------------------------------------------------------------------------
// signals
// ---------------------------------------------------------------------------

void EventLoop::handle_signals() {
  for (;;) {
    struct signalfd_siginfo si {};
    const ssize_t n = ::read(sig_fd_.get(), &si, sizeof(si));
    if (n == static_cast<ssize_t>(sizeof(si))) {
      if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) {
        if (shutdown_requested_) {
          second_signal_ = true;
          log_note("second signal: closing connections immediately");
        } else {
          shutdown_requested_ = true;
          log_note("shutdown requested: draining");
        }
      }
      continue;
    }
    const int e = errno;
    if (e == EINTR)
      continue;
    return; // EAGAIN: drained
  }
}

// ---------------------------------------------------------------------------
// interest management
// ---------------------------------------------------------------------------

void EventLoop::set_interest(const std::shared_ptr<Connection>& conn) {
  if (!conn->alive())
    return;
  const std::uint32_t want = conn->interest_mask();
  const std::uint32_t have = conn->applied_mask();
  if (want == have)
    return;

  struct epoll_event ev {};
  ev.events = want;
  ev.data.fd = conn->fd();
  if (::epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, conn->fd(), &ev) != 0) {
    const int e = errno;
    if (e == ENOENT || e == EBADF) {
      reap_.push_back(conn->fd());
      return;
    }
    log_io("epoll_ctl MOD", conn->peer(), e);
    return;
  }
  conn->set_applied_mask(want);

  // Edge-triggered caveat, the one that bites hardest: re-adding EPOLLIN does
  // not guarantee a fresh notification for bytes that arrived while EPOLLIN was
  // clear (there was no new "edge").  Any connection whose EPOLLIN we just
  // (re)enabled is therefore re-scanned explicitly after this event batch.
  if ((want & EPOLLIN) != 0 && (have & EPOLLIN) == 0)
    rescan_.push_back(conn);
}

void EventLoop::process_rescans() {
  std::vector<std::shared_ptr<Connection>> pending;
  pending.swap(rescan_);
  for (auto& conn : pending) {
    if (!conn->alive() || !conn->want_read())
      continue;
    conn->on_readable();
    if (!conn->alive() || conn->finished()) {
      reap_.push_back(conn->fd());
      continue;
    }
    set_interest(conn);
  }
}

void EventLoop::reap() {
  if (reap_.empty())
    return;
  for (const int fd : reap_) {
    const auto it = conns_.find(fd);
    if (it == conns_.end())
      continue;
    if (::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr) != 0 && errno != ENOENT && errno != EBADF)
      log_io("epoll_ctl DEL", it->second->peer(), errno);
    if (cfg_.verbose)
      log_note("closing " + it->second->peer() + " requests=" + std::to_string(it->second->requests_served()));
    conns_.erase(it); // may run ~Connection and close the socket
  }
  reap_.clear();
}

// ---------------------------------------------------------------------------
// timers / shutdown
// ---------------------------------------------------------------------------

void EventLoop::tick() {
  const auto now = Clock::now();

  if (accept_retry_) {
    accept_retry_ = false;
    accept_batch();
  }

  for (auto& kv : conns_)
    kv.second->on_tick(now);
  for (auto& kv : conns_) {
    if (kv.second->finished())
      reap_.push_back(kv.first);
  }
}

void EventLoop::maybe_begin_drain() {
  if (!shutdown_requested_ || draining_)
    return;
  draining_ = true;
  drain_deadline_ = Clock::now() +
                    std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(cfg_.shutdown_timeout_sec));
  {
    char line[192];
    std::snprintf(line, sizeof(line), "draining: %zu connections, %zu queued jobs, budget %.3fs",
                  conns_.size(), pool_.queue_size(), cfg_.shutdown_timeout_sec);
    log_note(line);
  }
  // The listener stays open on purpose: new clients get an immediate, explicit
  // 503 instead of hanging in the kernel backlog until they time out.
  for (auto& kv : conns_)
    kv.second->begin_draining();
  for (auto& kv : conns_) {
    if (kv.second->finished())
      reap_.push_back(kv.first);
  }
}

bool EventLoop::shutdown_complete() {
  if (!draining_)
    return false;

  if (second_signal_) {
    forced_shutdown_ = true;
    close_all();
    return true;
  }

  if (conns_.empty() && pool_.queue_size() == 0 && completions_pending() == 0)
    return true;

  if (Clock::now() >= drain_deadline_) {
    char line[192];
    std::snprintf(line, sizeof(line), "shutdown budget exhausted: forcing %zu connections closed",
                  conns_.size());
    log_note(line);
    exit_code_ = 0; // drained as far as the budget allowed, not an error
    forced_shutdown_ = true;
    close_all();
    return true;
  }
  return false;
}

void EventLoop::close_all() {
  for (auto& kv : conns_)
    kv.second->kill();
  for (auto& kv : conns_)
    reap_.push_back(kv.first);
  reap();
}

// ---------------------------------------------------------------------------
// main loop
// ---------------------------------------------------------------------------

int EventLoop::run() {
  const int epfd = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    log_io("epoll_create1", "startup", errno);
    return 1;
  }
  epfd_ = Fd(epfd);

  if (!setup_wake() || !setup_signals() || !setup_listener()) {
    exit_code_ = 1;
    pool_.stop();
    return exit_code_;
  }

  std::vector<struct epoll_event> events(static_cast<std::size_t>(kMaxEvents));
  last_tick_ = Clock::now();

  for (;;) {
    const int n = ::epoll_wait(epfd_.get(), events.data(), kMaxEvents, cfg_.tick_ms);
    if (n < 0) {
      const int e = errno;
      if (e == EINTR)
        continue; // a signal we do not care about (SIGPIPE is ignored anyway)
      log_io("epoll_wait", "loop", e);
      exit_code_ = 1;
      break;
    }

    for (int i_raw = 0; i_raw < n; ++i_raw) {
      const auto i = static_cast<std::size_t>(i_raw);
      const int fd = events[i].data.fd;
      const std::uint32_t ev = events[i].events;

      if (fd == wake_fd_.get()) {
        drain_wake();
        continue;
      }
      if (fd == sig_fd_.get()) {
        handle_signals();
        continue;
      }
      if (fd == listen_fd_.get()) {
        accept_batch(); // also handles the draining case (503 + close)
        continue;
      }

      const auto it = conns_.find(fd);
      if (it == conns_.end()) {
        // Stale event for a connection we already destroyed.  The kernel drops
        // the registration on close anyway; be explicit about it.
        ::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
        continue;
      }
      std::shared_ptr<Connection> conn = it->second; // alive for the whole callback

      if ((ev & (EPOLLERR | EPOLLHUP)) != 0) {
        conn->on_hangup(ev);
        if (!conn->alive() || conn->finished()) {
          reap_.push_back(fd);
          continue;
        }
        set_interest(conn);
        continue;
      }
      if ((ev & EPOLLIN) != 0)
        conn->on_readable();
      if (conn->alive() && (ev & EPOLLOUT) != 0)
        conn->on_writable();

      if (!conn->alive() || conn->finished()) {
        reap_.push_back(fd);
        continue;
      }
      set_interest(conn);
    }

    // Completions may also have arrived after the eventfd was drained above.
    drain_completions();
    process_rescans();
    reap();
    maybe_begin_drain();

    const auto now = Clock::now();
    if (now - last_tick_ >= std::chrono::milliseconds(cfg_.tick_ms)) {
      last_tick_ = now;
      tick();
      reap();
    }

    if (shutdown_complete())
      break;
  }

  // Normal path: accepted jobs are always finished before the workers go away,
  // and results for connections we already closed are dropped by the weak_ptr
  // check in drain_completions().
  // Forced path (budget exhausted / second signal): do not let a wedged handler
  // hold the process open.  Detach the stragglers and deliberately leak the
  // wake descriptor, so a detached worker can never write() into a descriptor
  // number that has been recycled by another process/thread.
  if (forced_shutdown_) {
    if (!pool_.stop_forced(250)) {
      log_note("hard shutdown: detaching worker(s) still running application work");
      wake_fd_.release();
    }
  } else {
    pool_.stop();
  }
  {
    std::lock_guard<std::mutex> lock(comp_mu_);
    completions_.clear();
  }
  close_all();
  log_stats();
  return exit_code_;
}

} // namespace http
