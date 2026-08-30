# cpp-http-server

HTTP/1.0 + HTTP/1.1 server for Linux: one `epoll(7)` loop in **edge-triggered**
mode over non-blocking sockets, plus a fixed-size worker pool with a bounded job
queue. Workers never touch epoll; they post finished responses back to the loop
through an `eventfd`.

Written to be auditable rather than clever: ~1900 lines of C++20, seven source
files, three mutexes total, no smart-pointer cycles, no thread-local state.

## Layout

```
CMakeLists.txt
src/
  main.cpp          SIGPIPE ignore, signal masking, argv, run loop
  config.{h,cpp}    Config + getopt_long CLI
  common.h          Request/Response, ByteBuf (read-cursor buffer), helpers
  http_common.cpp   validation helpers, response serialisation, error responses
  http_parser.{h,cpp}  incremental request parser (no sockets, unit-testable)
  thread_pool.{h,cpp}  pthread worker pool, bounded queue, try_submit()
  handler.{h,cpp}   the "application": routes, runs on worker threads
  connection.{h,cpp}  one client socket: parse -> submit -> write, backpressure
  event_loop.{h,cpp}  epoll + accept4 + eventfd + signalfd, owns all Connections
  fd.h              RAII descriptor
tests/
  parser_tests.cpp        95 checks, no sockets
  test_http_server.py     37 integration tests (real process, raw sockets)
  run_tests.sh            build + unit + integration
bench/bench.sh            wrk -> ab -> built-in python client
```

## Build

Requires Linux (it uses `epoll`, `accept4`, `eventfd`, `signalfd`) and a C++20
compiler; CMake links pthreads through `Threads::Threads`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
```

Compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wold-style-cast -Wnon-virtual-dtor -Wcast-qual -Wstrict-aliasing=2` and clean
(no warnings) with GCC 13 on Ubuntu 24.04.

## Run

```sh
./build/http_server                                   # [::]:8080, dual stack
./build/http_server --bind 0.0.0.0 --port 8080 -v
./build/http_server --help
```

Useful flags: `--workers`, `--queue-size`, `--max-connections`, `--max-body`,
`--max-headers`, `--max-request-line`, `--max-output`, `--idle-timeout`,
`--shutdown-timeout`, `--port-file <path>` (writes the bound port; used by the
tests when `--port 0`).

Routes for manual poking: `/`, `/health`, `/info` (request as JSON),
`/echo` (echoes the body), `/big/<n>` (n-byte body), `/slow/<ms>` (blocks a
worker), `/close`, `/boom` (500).

## Test

```sh
tests/run_tests.sh                 # configure + build + unit + integration
tests/run_tests.sh --no-build -k shutdown   # only shutdown tests
./build/parser_tests               # parser unit tests
```

Sanitizer builds (the lifetime/race review was validated this way, not by
eyeballing):

```sh
cmake -B build-asan -DENABLE_ASAN=ON && cmake --build build-asan -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=1 python3 tests/test_http_server.py --binary ./build-asan/http_server

cmake -B build-tsan -DENABLE_TSAN=ON && cmake --build build-tsan -j"$(nproc)"
TSAN_OPTIONS=halt_on_error=0 python3 tests/test_http_server.py --binary ./build-tsan/http_server
```

On Ubuntu 24.04/aarch64 TSan needs ASLR off, otherwise it aborts at startup with
`FATAL: ThreadSanitizer: unexpected memory mapping`. Wrap the binary:
`exec setarch aarch64 -R ./build-tsan/http_server "$@"` and point `--binary` at
the wrapper.

## Benchmark

```sh
bench/bench.sh --port 8080 --conns 50 --duration 10 --path /
bench/bench.sh --port 8080 --conns 50 --duration 10 --path /big/65536
bench/bench.sh --external --port 8080 --duration 10     # against a server you started
```

Uses `wrk` if present, else `ab`, else a threaded Python keep-alive client.
Measured here with the Python client (so the client, not the server, is the
bottleneck): ~18k req/s keep-alive on tiny responses and ~720 MB/s on 64 KiB
responses, 0 errors, `wrk`/`ab` will show considerably more.

## Design

### Thread model

* **Event-loop thread** — the only thread that ever touches a `Connection`, its
  buffers, or epoll. Accepts, reads, parses, writes, times out, reaps.
* **Worker threads** — `handle_request(Request) -> Response`. They may block
  (`/slow/<ms>` proves it); they cannot block the loop. The only thing a worker
  does with the loop is `post_completion()`: take a mutex, push, release,
  `write()` the eventfd (deliberately outside the lock).

`try_submit()` never waits for queue space. A full queue is overload, and
overload is a `503` on the loop — the loop cannot be stalled by application
work.

### Lifetime (no use-after-free)

`EventLoop` owns connections in `unordered_map<fd, shared_ptr<Connection>>`.
The job handed to a worker captures a `weak_ptr<Connection>` plus
`(conn_id, seq)`. The `weak_ptr` is `lock()`ed **only** on the loop thread, in
`drain_completions()`; if the client vanished the result is dropped, and the
`seq` check discards late or duplicated results. Connections are destroyed in
one place (`reap()`), after the epoll batch, so nothing can be freed while a
pointer to it is live in the event loop. `Fd` closes exactly once, which also
removes the double-close/recycled-fd hazard.

### Edge-triggered correctness

Every registration uses `EPOLLET`, and every read/write path drains to
`EAGAIN`/`EWOULDBLOCK`. Three places handle the traps that come with that:

1. **Re-enabling `EPOLLIN` is not an edge.** When a connection stops reading
   (request in flight, or output backpressure) and later wants to read again,
   bytes may have arrived in between with no new edge to report. `set_interest()`
   records every connection whose `EPOLLIN` bit was just (re)enabled and the
   loop re-scans those connections after the batch (`process_rescans()`).
2. **Accept queue.** `accept4()` loops to `EAGAIN`, capped at 256 per pass; if
   the cap is hit, a flag makes the tick timer re-run `accept_batch()`, so the
   listener cannot go quiet. `EMFILE`/`ENFILE` uses the same retry path.
3. **eventfd / signalfd** are drained in loops too (`EINTR` retried, `EAGAIN`
   means done). Completions are also drained once per iteration after the batch,
   which closes the window between draining the eventfd and calling
   `epoll_wait()`.

`EPOLLERR` kills a connection immediately; `EPOLLHUP` flushes what is already
queued (best effort) and then closes.

### Partial reads and writes

`ByteBuf` holds input and output with a read cursor; consumed bytes are dropped
only when they are at least half of the buffer, so streaming an 8 MiB body is
`O(n)`, not the `O(n^2)` you get from `erase(0, n)` per read. Writes loop
`send(MSG_NOSIGNAL)` until empty or `EAGAIN`; `EPOLLOUT` is registered only
while bytes are pending (otherwise the loop would spin). `EINTR` is retried in
both directions; `EPIPE`/`ECONNRESET` just closes the connection (SIGPIPE is
`SIG_IGN` *and* `MSG_NOSIGNAL` is used, so a vanished client cannot kill the
process).

### Backpressure (`Connection::on_readable`)

* The read loop stops whenever `want_read()` goes false, and `want_read()` is
  false while a request is in flight, while the output buffer is at
  `--max-output`, after EOF, and during draining.
* Exactly **one** request per connection is in flight. Pipelined requests stay
  in the socket/`in_` buffer, so an over-pipelining client is throttled by its
  own TCP window instead of by our memory.
* Input is capped after **every** `recv()`:
  `bytes_consumed + in_.size() <= --max-request`, else `413` and close. Client
  driven allocation is therefore bounded by config, never by client demand.
* When output exceeds the cap we drop `EPOLLIN` (read-stoppage is the
  flow-control signal); a client that neither reads nor goes away is closed by
  the write-stall branch of `on_tick()`.

### Framing and parser rules

`Content-Length` is rejected when duplicated (even identical values), when
list-valued, malformed, signed, or over `--max-body`; it is rejected together
with `Transfer-Encoding`. `Transfer-Encoding` must be exactly one field with
exactly the single token `chunked`, on HTTP/1.1 only. Chunked decoding is
incremental, validates chunk-size lines, tolerates chunk extensions, requires
the CRLF after chunk data, and accepts (then ignores) trailers. Strict CRLF:
a bare LF is `400`. Request targets must be origin-form or `*`:
absolute-form (`http://host/p`), authority-form, protocol-relative (`//host/p`),
and fragments are rejected. Header names must be RFC 7230 tokens; values may not
contain control characters, so CR/LF header injection is impossible; obs-fold is
rejected. Request line, header block and body each have their own limit.
Unknown-but-syntactically-valid methods are `405` with `Allow` (and their body is
still framed and consumed, so the stream stays resynchronised).
`Expect: 100-continue` emits one `100 Continue` before the body is parsed.
Responses always carry `Content-Length` (never chunked, so HTTP/1.0 clients are
happy); `HEAD` gets the correct `Content-Length` with zero body bytes; `204`
gets no `Content-Length` at all.

`Host` and every other header are treated as untrusted data: never used for
routing, never echoed back unsanitised (`sanitize_header_value()` strips
CR/LF/control bytes on the way out, and `/info` JSON-escapes everything).

### Deliberate deviations (documented, not silent)

* **Duplicate identical `Content-Length` is `400`.** RFC 7230 permits identical
  duplicates; they are also the request-smuggling primitive, so we refuse all of
  them.
* **`Connection` options other than `keep-alive`/`close` are `400`**, including
  `upgrade`: ignoring an upgrade request while keeping the socket would be a
  protocol-confusion hazard.
* **Oversized request line is `413`**, not `414`: the requirement list only
  covers 400/404/405/408/413/500/503.
* **`Transfer-Encoding` on HTTP/1.0 is `400`** (chunked is a 1.1 feature).
* **An in-flight request is never killed by the idle timeout.** The timeout
  covers client silence while we wait for a request; long handler work is
  application policy (covered by `test_19b`).
* **Hard shutdown can detach a wedged worker.** On the second SIGINT/SIGTERM, or
  when the shutdown budget expires with jobs still running, the loop closes every
  connection and joins workers with a 250 ms bound; a worker still inside
  application code is detached and the eventfd is intentionally leaked
  (`Fd::release()`) so the detached thread can never `write()` into a descriptor
  number that has been recycled. Normal (single-signal) shutdown is lossless:
  accepted jobs are drained before workers are joined.

### Shutdown

`SIGINT`/`SIGTERM` are blocked in `main()` before the pool exists and delivered
through `signalfd` inside the same epoll set (no self-pipe, no signal handlers
touching state). Drain keeps the listener open and answers new connections with
`503` (rather than letting clients hang in the backlog), stops reading from
existing connections, finishes and flushes in-flight work, and exits `0`. Idle
connections close immediately; the budget is `--shutdown-timeout`.
