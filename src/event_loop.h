#pragma once
//
// The event loop: epoll(EPOLLET) + accept4 + eventfd + signalfd, and the sole
// owner of every Connection.
//
// Invariants:
//   * Only this thread touches Connection state or epoll.  Worker threads hand
//     results back through post_completion(), which is the only method here
//     that is safe to call from another thread (besides wake()).
//   * Nothing in the loop blocks on application work.  A full worker queue is
//     answered with 503, never with a wait.
//   * Connections are destroyed only inside reap(), never in the middle of
//     iterating the epoll result set.
//

#include "common.h"
#include "config.h"
#include "connection.h"
#include "fd.h"
#include "thread_pool.h"

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace http {

class EventLoop {
public:
  explicit EventLoop(Config cfg);
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // Blocks until shutdown completes.  Returns the process exit code.
  int run();

  // --- safe from worker threads -------------------------------------------
  void post_completion(Completion&& completion);
  void wake() noexcept;

  // --- accessors -----------------------------------------------------------
  ThreadPool& pool() noexcept { return pool_; }
  const Config& config() const noexcept { return cfg_; }
  std::size_t connection_count() const noexcept { return conns_.size(); }

  static std::uint64_t next_connection_id() noexcept {
    return next_conn_id_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  // --- logging / counters (event-loop thread) ------------------------------
  void log_io(std::string_view what, std::string_view peer, int err) noexcept;
  void log_bad_request(std::string_view peer, std::string_view why) noexcept;
  void log_note(std::string_view what) noexcept;
  void note_request_received() noexcept { ++requests_received_; }
  void note_overload_rejection() noexcept { ++overload_rejections_; }
  void note_timeout() noexcept { ++timeouts_; }

private:
  using Clock = std::chrono::steady_clock;

  bool setup_wake();
  bool setup_signals();
  bool setup_listener();
  void write_port_file(std::uint16_t port);

  void accept_batch();
  void reject_connection(int fd, std::string_view reason);
  void drain_wake();
  void drain_completions();
  void handle_signals();
  void set_interest(const std::shared_ptr<Connection>& conn);
  void process_rescans();
  void reap();
  void tick();
  void maybe_begin_drain();
  bool shutdown_complete();
  void close_all();
  std::size_t completions_pending();
  void log_stats() noexcept;

  Config cfg_;
  Fd epfd_;
  Fd listen_fd_;
  Fd wake_fd_;
  Fd sig_fd_;
  ThreadPool pool_;

  std::unordered_map<int, std::shared_ptr<Connection>> conns_;
  std::vector<int> reap_;
  std::vector<std::shared_ptr<Connection>> rescan_;

  std::mutex comp_mu_;
  std::deque<Completion> completions_;

  bool shutdown_requested_ = false;
  bool second_signal_ = false;
  bool draining_ = false;
  bool forced_shutdown_ = false; // budget exhausted or second signal
  bool accept_retry_ = false;
  int exit_code_ = 0;
  std::uint16_t bound_port_ = 0;
  Clock::time_point drain_deadline_{};
  Clock::time_point last_tick_{};

  std::uint64_t accepted_ = 0;
  std::uint64_t rejected_connections_ = 0;
  std::uint64_t requests_received_ = 0;
  std::uint64_t overload_rejections_ = 0;
  std::uint64_t timeouts_ = 0;

  static std::atomic<std::uint64_t> next_conn_id_;
};

} // namespace http
