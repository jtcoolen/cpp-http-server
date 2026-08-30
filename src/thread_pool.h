#pragma once
//
// Fixed-size worker pool with a *bounded* job queue.
//
// Contract that the event loop depends on:
//   * try_submit() never waits for queue space.  It takes the queue mutex for
//     the duration of a push_back and returns false when the queue is full or
//     the pool is stopping.  A full queue is overload, and overload is answered
//     with 503 by the caller - the event loop is never blocked by application
//     work.
//   * Workers only run application handlers.  They never touch epoll, sockets
//     or Connection objects; results travel back through
//     EventLoop::post_completion().
//

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include <pthread.h>

namespace http {

class ThreadPool {
public:
  ThreadPool(std::size_t threads, std::size_t max_queue);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  bool try_submit(std::function<void()> job);

  // Stop accepting work, let the workers drain what is already queued, then
  // join them.  Safe to call more than once; the destructor calls it.
  void stop();

  // Hard shutdown: stop, wait at most `wait_ms` for the workers, then detach
  // whatever is still running.  Returns true if everything was joined.  Only
  // used when the shutdown budget was exhausted or a second signal arrived.
  bool stop_forced(long wait_ms);

  std::size_t queue_size() const;
  std::size_t worker_count() const noexcept { return threads_.size(); }
  bool stopping() const;

private:
  static void* trampoline(void* arg);
  void run();

  mutable std::mutex mu_;
  std::condition_variable have_work_;
  std::deque<std::function<void()>> queue_;
  std::size_t max_queue_;
  bool stopping_ = false;
  bool joined_ = false;
  std::vector<pthread_t> threads_;
};

} // namespace http
