// _GNU_SOURCE is provided by the build (target_compile_definitions); it must
// precede every system header because pthread_setname_np() is guarded by it.
#include <errno.h>

#include <chrono>
#include <exception>
#include <thread>
#include <utility>

#include "thread_pool.h"

namespace http {

ThreadPool::ThreadPool(std::size_t threads, std::size_t max_queue) : max_queue_(max_queue) {
  if (max_queue_ == 0)
    max_queue_ = 1;
  if (threads == 0)
    threads = 1;
  threads_.reserve(threads);
  for (std::size_t i = 0; i < threads; ++i) {
    pthread_t t{};
    const int rc = ::pthread_create(&t, nullptr, &ThreadPool::trampoline, this);
    if (rc != 0) {
      // Could not start every worker.  Join what we did start and keep going
      // with fewer threads rather than crashing: capacity is a policy, not an
      // invariant.  (stop() is idempotent, and the destructor will not re-join
      // threads that were never created.)
      break;
    }
    threads_.push_back(t);
  }
}

void* ThreadPool::trampoline(void* arg) {
  static_cast<ThreadPool*>(arg)->run();
  return nullptr;
}

ThreadPool::~ThreadPool() { stop(); }

bool ThreadPool::try_submit(std::function<void()> job) {
  if (!job)
    return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_ || threads_.empty())
      return false;
    if (queue_.size() >= max_queue_)
      return false; // overload: caller answers 503
    queue_.push_back(std::move(job));
  }
  have_work_.notify_one();
  return true;
}

void ThreadPool::run() {
  ::pthread_setname_np(pthread_self(), "http-worker");
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      have_work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        // stopping_ and drained: exit.  Queued work is always finished first,
        // which is what makes graceful shutdown lossless for accepted jobs.
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      job();
    } catch (const std::exception& e) {
      // A worker must never die because of application work: it would slowly
      // drain the pool.  Swallowing here is deliberate; the handler itself is
      // responsible for turning its own errors into a 500.
      (void)e;
    } catch (...) {
      // ditto
    }
  }
}

void ThreadPool::stop() {
  std::vector<pthread_t> to_join;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_ && joined_)
      return;
    stopping_ = true;
    to_join.swap(threads_);
    joined_ = true;
  }
  have_work_.notify_all();
  for (pthread_t t : to_join)
    ::pthread_join(t, nullptr);
}

bool ThreadPool::stop_forced(long wait_ms) {
  std::vector<pthread_t> to_join;
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
    to_join.swap(threads_);
    joined_ = true;
  }
  have_work_.notify_all();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  bool all_joined = true;
  for (pthread_t t : to_join) {
    void* retval = nullptr;
    bool joined = false;
    while (true) {
      const int rc = ::pthread_tryjoin_np(t, &retval);
      if (rc == 0 || rc != EBUSY) {
        joined = true; // joined, or unrecoverable: treat as gone
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        ::pthread_detach(t); // still executing application work
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    all_joined = all_joined && joined;
  }
  return all_joined;
}

std::size_t ThreadPool::queue_size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_.size();
}

bool ThreadPool::stopping() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stopping_;
}

} // namespace http
