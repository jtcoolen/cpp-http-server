#pragma once
//
// Minimal RAII wrapper for a file descriptor.
//
// Everything in this project that owns a descriptor owns it through this type,
// so "who closes this fd?" always has one answer: the last owner's destructor.
// That is the main defence against double-close (which, on Linux, is a genuine
// use-after-free hazard because a closed descriptor number can immediately be
// handed out again by accept()).
//

#include <unistd.h>

#include <utility>

namespace http {

class Fd {
public:
  Fd() noexcept = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}
  ~Fd() { reset(); }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return valid(); }

  // Close the descriptor (if any) and become empty.  Idempotent.
  void reset() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  // Give up ownership without closing (caller must close exactly once).
  int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

private:
  int fd_ = -1;
};

} // namespace http
