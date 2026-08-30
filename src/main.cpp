//
// Entry point: ignore SIGPIPE, block SIGINT/SIGTERM before any thread exists so
// signalfd() is the only way to observe them, parse the command line, run the
// event loop.
//

#include "config.h"
#include "event_loop.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>

#include <exception>
#include <string>

int main(int argc, char** argv) {
  // SIGPIPE must never terminate the process.  send() also passes MSG_NOSIGNAL,
  // so we are covered twice: a write to a socket whose peer vanished returns
  // EPIPE and we close the connection.
  if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    ::perror("signal(SIGPIPE)");
    return 1;
  }

  // Block these before creating the worker threads: the mask is inherited, so
  // no thread can ever take a default-disposition exit, and signalfd() gets to
  // handle them inside the epoll set (no self-pipe, no async-signal-callback).
  sigset_t mask;
  ::sigemptyset(&mask);
  ::sigaddset(&mask, SIGINT);
  ::sigaddset(&mask, SIGTERM);
  if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
    ::perror("pthread_sigmask");
    return 1;
  }

  http::Config cfg;
  std::string err;
  bool wants_exit_success = false;
  if (!http::parse_args(argc, argv, cfg, err, wants_exit_success)) {
    std::fputs(err.c_str(), wants_exit_success ? stdout : stderr);
    return wants_exit_success ? 0 : 2;
  }

  try {
    http::EventLoop loop(std::move(cfg));
    return loop.run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[server] fatal: %s\n", e.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "[server] fatal: unknown exception\n");
    return 1;
  }
}
