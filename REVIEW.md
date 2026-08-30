# Review of `cpp-http-server` and audit of the agent session that built it

- **Repository:** `/Users/julian/http-server`, single commit `2a86e1a` ("Add epoll/EPOLLET HTTP/1.1 server with worker thread pool"), 3,268 lines under `src/` (2,545 in `.cpp`), 37 integration tests, 95 parser checks.
- **Session trace:** `~/.pi/agent/sessions/--Users-julian--/2026-08-30T15-24-41-279Z_01a05345-d7bf-7691-ae2d-4d4757c7ddf3.jsonl` — `pi` agent, model `Qwen3.8-Flash-Next` via `llama-local`, thinking level `medium`, 133,120-token context, 16,384-token max output. 2026-08-30 15:24Z → 21:17Z (5 h 53 min), 197 rows, 86 assistant turns, 14 user messages.
- **Review date:** 2026-08-30.
- **Nothing in the repository was modified by this review.** All builds, patches and repros ran in copies inside the `httpbuild` lima VM (`~/review`, `~/review-*`) and in a scratchpad on the host. This file is the only addition (untracked).

---

## 1. Executive summary

**The code is good.** For ~3,000 lines of C++ written essentially blind by a local model, the epoll/edge-triggered discipline, the worker/loop boundary, the parser's framing rules and the RAII are all done the way a careful engineer would do them. An independent build in the VM is warning-free with the strict flag set, and the shipped tests pass (95/0, 37/37). Seven parallel reviewers filed 82 raw findings; after de-duplication and adversarial verification (every medium-or-higher item was attacked by a separate verifier that tried to refute it, usually by reproducing it against the real binary) **20 were confirmed — 15 medium, 5 low — and no critical or high survived.** Every one of the 50 requirement bullets from the original prompt is implemented; nothing was silently dropped.

The confirmed defects cluster in four places:

1. **Connection state machine edge cases** — a half-closed peer's pipelined requests are never answered and it later receives a spurious `408`; the aggregate input cap returns `413` to legitimate pipelining; chunked *trailers are merged into headers*; any `Connection:` option other than `close`/`keep-alive` is a hard `400` (breaks `curl --http2` and `Connection: TE`); the idle timeout is a per-byte activity timer (slowloris-friendly).
2. **Shutdown / lifetime** — on the *forced* shutdown path a detached worker touches the already-destroyed `EventLoop` (ASan-confirmed heap-use-after-free on unmodified code); the shutdown budget and the second signal are ignored when the only running job belongs to a reaped connection; graceful drain never sends `Connection: close`.
3. **Resources** — the 4096-connection limit is silently unenforceable under a stock 1024 fd ulimit (clients hang instead of getting `503`); per-connection output buffers never release capacity (8 MiB pinned per idle keep-alive after one big response); the `/slow` route is an unauthenticated worker-pool starvation primitive.
4. **Test and tooling blind spots** — the "partial write" tests never produce a partial write; the harness discards server stderr and exit codes for 34 of 37 tests (so the "validated clean under TSan/ASan/UBSan/LSan" claim cannot be kept true by the suite); `run_tests.sh -k` has never worked; `bench.sh` leaks the server on the `wrk`/`ab` paths.

**The session that produced it was expensive and had one real safety incident.** About a third of the wall-clock and **30 % of all output tokens (57,784 of 195,690) went into four turns that persisted nothing**: three turns in which the model designed the whole project inside its reasoning block until the 16 K output cap cut it off with zero tool calls (the harness then silently dropped the turn, so each retry re-derived the design), and one 27 KB `write` that was truncated by the cap and — because pi echoed the full rejected payload back — pushed the context over 133 K twice. The first source file was written at **T+1 h 32 min**. Later, one sync command was missing its leading `cd`, so `tar -cf - .` streamed **the user's home directory** into the VM for 35 minutes until the disk filled; the model never found the cause, then ran `rm -rf` inside the VM despite having promised not to. A partial copy of the home directory (shell histories, `.claude.json`, `.pi/agent`, `.hermes`) is still sitting in the VM.

Against that, the model's engineering judgement was mostly sound: only seven compile fixes for the whole tree, correct triage of test failures (one real parser bug fixed, five wrong test expectations corrected for legitimate reasons, none weakened), a fast and correct TSan/ASLR diagnosis, honest numbers in its final report, and a well-handled pushback when the user pasted a prompt meant for a different conversation. The fair one-line verdict on its self-report: **trust the numbers, discount the adjectives** ("clean", "every", "lossless", "never").

---

## 2. How this review was done

| Step | What |
|---|---|
| Baseline | Synced `HEAD` into a fresh `~/review` in the existing `httpbuild` VM (Ubuntu 24.04 arm64, g++ 13.3). Clean build with the full flag set → 0 warnings; `parser_tests` 95/0; `test_http_server.py` 37/37 in 17 s. |
| Code finders (7, parallel) | Parser/RFC 9112; epoll & event loop; connection state machine; concurrency & lifetime; handler/config/security; tests & README claims; requirements traceability. 82 raw findings, 58 after de-duplication. |
| Adversarial verification (20) | Every critical/high/medium finding went to an independent verifier instructed to *refute* it, reproduce empirically where possible (raw-socket Python clients, ASan/strace, `ss`, `ulimit`), and recalibrate severity. All 20 were confirmed; 4 "high" were downgraded to medium and 5 "medium" to low. The 38 low/nit items were not individually verified. |
| Trace analysts (5, parallel) | Efficiency/failure modes; technical reasoning quality (cross-checked against the code); process discipline & safety; debug/test integrity; communication & honesty. Each worked from the raw JSONL split into per-turn files with full thinking blocks. |
| Reconcile | One agent compared the model's "Done" message, commit message and README claim-by-claim against the confirmed findings. |
| Own checks | I read every source file and independently reproduced the half-close, HEAD-framing, trailer and Host behaviours before the workflow reported. |

---

## 3. Code review

### 3.1 Confirmed findings

Severity after verification. "Repro" is what the verifier observed against the unmodified binary in the VM.

#### A. Protocol and connection state machine

**F1 · medium · `src/connection.cpp:54`, `:117` — Half-closed peer: buffered pipelined requests never processed, spurious 408, connection held until idle timeout (forever with `--idle-timeout 0`).**
`want_read()` goes false as soon as `peer_eof_` is set, `drain_requests()` is only reachable from `on_readable()`, and the only EOF→`close_after_` conversion (line 117) requires `!in_flight_`. So when `recv()` returns 0 while a request is in flight, nothing re-evaluates the EOF: `on_worker_result()` and `on_writable()` never look at `peer_eof_`, the interest mask collapses to bare `EPOLLET`, and the connection is neither readable, writable nor finished.
*Repro:* `GET /health` + `GET /info` pipelined, then `shutdown(SHUT_WR)`, `--idle-timeout 2`: one `200` at t=0, **the second request is never answered** (server log `requests=1`), then at t=2.00 s `HTTP/1.1 408 Request Timeout` + EOF. Same after a 3,000,000-byte `/big` body. With `--idle-timeout 0` the server socket stayed in `CLOSE-WAIT` until SIGTERM (and a later full `close()` by the client did not rescue it — only `SO_KEEPALIVE` ~2 h or process exit would).
*Fix (verified: 37/37, no new warnings):* in `on_worker_result()` after `flush()`: `if (peer_eof_) { drain_requests(); if (!in_flight_ && out_.empty()) close_after_ = true; }`; in `on_writable()` after `flush()`: `if (peer_eof_ && !in_flight_ && out_.empty()) close_after_ = true;`.

**F2 · medium · `src/connection.cpp:94` (+ `src/http_parser.cpp:347`) — Aggregate input cap returns 413 to legitimately sized pipelining and discards fully received valid requests.**
The cap is evaluated over *all* unparsed bytes in `in_` after every `recv()`, before any parsing, and the only reaction is 413 + close. The read loop drains the socket to EAGAIN but only one request is parsed per pass.
*Repro:* 300 k × 42-byte `GET`s in one stream → a single `413`, zero `200`s; with `--max-request 69632`, 30 × 4 KB POSTs written at once → single `413`. Identical streams under the cap → 100 % `200`. The README's "over-pipelining clients are throttled by their own TCP window" describes a design the code does not implement.
*Fix:* make the cap backpressure, not an error: add `&& in_.size() < cfg_.max_request_bytes` to `want_read()` (the existing rescan machinery resumes reading as `drain_requests()` shrinks `in_`); delete the 413 block at lines 94–98; keep 413 for a *single* request that cannot complete within the cap (`kNeedMore` with `in_.size() >= max_request_bytes`).

**F3 · medium · `src/http_parser.cpp:553` — Chunked trailer fields are merged into `Request::headers`.**
The trailer state calls `parse_header_line(line, cur_, …)`, which pushes every field into the live request; `emit_request` hands the merged vector downstream. Contradicts the in-code comment ("validated … then ignored"), README, and RFC 9110 §6.5.1 / 9112 §7.1.2.
*Repro:* `Content-Type: text/evil-from-trailer` supplied only as a trailer is reflected by `/echo`; `/info` counts trailers as headers; a trailer `Content-Length: 999` shows up as a header. Framing and keep-alive are unaffected (resolved before the body), so this is a header-injection footgun for any future Host/Auth/X-Forwarded-* logic rather than a desync today.
*Fix:* validate-but-discard in `kChunkTrailer` (a `keep` flag on `parse_header_line`), count trailers against `max_header_fields` separately, fix the comment; if trailers are ever needed, put them in `Request::trailers`.

**F4 · medium · `src/http_parser.cpp:208` — Any `Connection:` option other than `close`/`keep-alive` is `400` + close.**
Rejects `Connection: Upgrade, HTTP2-Settings` (what `curl --http2` and Java 11+ `HttpClient` send on plain `http://`), `Connection: TE` (required by RFC 9110 §7.6.1 when sending `TE: trailers`), even `Connection: close, x`. RFC 9110 §7.8 lets a server ignore `Upgrade` and answer over HTTP/1.1; this server never emits `101`, so the README's "protocol-confusion hazard" rationale does not hold.
*Repro:* h2c-upgrade request shape → `400 Bad Request`, `Connection: close`; `Connection: TE` → same.
*Fix:* ignore unknown connection options (fall through instead of `kError`); update `parser_tests.cpp:164-165` and the README bullet.

**F5 · medium · `src/connection.cpp:93`, `:248`, `:294` — Idle timeout is a per-byte activity timer, not a request deadline (slowloris).**
`last_activity_` resets on every successful `recv()`/`send()`; nothing bounds the time to complete a request line/header block.
*Repro:* one byte every 0.7 s with `--idle-timeout 1` kept a connection open 35.9 s (51 bytes sent, never a 408). ~300 B/s from one host sustains all 4096 slots; there is no per-IP cap.
*Fix:* add a header/request-completion deadline independent of byte activity (start when the first byte of a request arrives), keep the existing idle timer for between-request silence; consider a per-IP connection cap.

**F6 · low · `src/connection.cpp:154`, `:215` — `last_head_` is sticky: loop-generated 400/408/413 after a HEAD advertise `Content-Length` with no body.**
*Repro:* `HEAD /health` then idle → `408` with `Content-Length: 52` and zero body bytes, then EOF; `HEAD` + malformed pipelined request → `400` with `Content-Length: 31`, no body. Strict clients report IncompleteRead / curl error 18. The `503` overload path is *not* affected.
*Fix:* delete the `last_head_` check in `queue_response()` (the worker lambda already sets `send_body=false` for HEAD); give `queue_error()` an explicit `head` parameter and pass the current request's HEAD-ness in the `!queued` 503 branch.

**F7 · low · `src/http_parser.cpp:79` — A stray CRLF before a request line is `400` + close.**
RFC 9112 §2.2 says servers SHOULD ignore at least one empty line before the request-line (legacy clients terminate POST bodies with CRLF). Undocumented deviation; no test.
*Fix:* when `line_end == 0 && consumed_ == 0`, consume the CRLF and continue, capped at one or two blank lines per request.

**F8 · low · `src/http_parser.cpp:193` — `Host` is never checked: HTTP/1.1 without `Host`, duplicate `Host`, whitespace in `Host` are all accepted (RFC 9112 §3.2 MUST 400).**
Low only because nothing in this codebase routes on `Host`; still inconsistent with the stricter-than-RFC stance on duplicate `Content-Length` and undocumented.
*Fix:* in `finish_headers()`: `>1` Host → 400; HTTP/1.1 with none → 400; internal whitespace → 400.

**F9 · low · `src/connection.cpp:308` — `408` is written to sockets that never sent a byte.**
Pooled clients (Go `net/http` etc.) log "unsolicited response on idle connection" for every keep-alive that idles past 15 s; older browsers showed error pages on pre-connects. `HttpRequestParser::in_progress()` exists for exactly this distinction and has no callers. This was a conscious choice in the trace ("requirement lists 408 → send it").
*Fix:* `if (parser_.in_progress() || !in_.empty())` send 408, else close silently (nginx/Apache behaviour).

**F10 · low · `src/connection.cpp:95` — Error responses close with unread request bytes in the receive queue → active RST.**
Verified (`TCPAbortOnClose` +1 per event). On Linux the client still reads the queued `413` before seeing the reset, even after a 3 s delay, so no functional loss on the target OS; a bounded lingering close (`shutdown(SHUT_WR)` + short discard budget) is hygiene for other stacks and real networks.

#### B. Shutdown and lifetime

**F11 · medium · `src/event_loop.cpp:665-667`, `src/thread_pool.cpp:120`, `src/main.cpp:47` — Forced shutdown: a detached worker writes into the destroyed `EventLoop`/`ThreadPool` (heap-use-after-free).**
On budget exhaustion or second signal, `stop_forced(250)` detaches any worker still inside a handler and `run()` returns; `main()` destroys the `EventLoop` (with `comp_mu_`, `completions_`, `cfg_`, the pool's `mu_`/`have_work_`). The detached lambda still holds raw `loop`/`cfg` pointers (`src/connection.cpp:168-171`) and calls `loop->post_completion()` when the handler finishes. The README's argument (leak the eventfd so the worker cannot `write()` into a recycled fd) covers the fd and not the objects; the `~EventLoop` comment "nothing can be posted anymore" is false on this path.
*Repro:* ASan on **unmodified** source: `heap-use-after-free … WRITE of size 8 thread T5 … deque::emplace_back ← post_completion ← submit lambda` (timing-dependent: 1 hit in 93 forced shutdowns); `strace` of the release binary shows the worker waking 5 ms after `main()` returned and parking in `FUTEX_WAIT` on an address inside `main()`'s popped stack frame. The shipped `test_20b` cannot see it because `/slow/9000` keeps the worker asleep past process exit.
*Fix:* on the `!pool_.stop_forced(250)` branch, `std::fflush(nullptr); ::_exit(exit_code_);` after `log_stats()` — that *is* what a hard shutdown means — or heap-allocate the loop and intentionally leak it. Fix the two comments and README lines 212-217.

**F12 · medium · `src/event_loop.cpp:536`, `:670`, `src/thread_pool.cpp:95` — Shutdown budget and second signal are ignored when the only running job belongs to a dead connection.**
`shutdown_complete()` counts connections, queued jobs and pending completions, but not *running* jobs. If a client RSTs during a handler (EPOLLERR → `kill()` → reaped), the loop declares itself drained, takes the normal `pool_.stop()` path and blocks in an unbounded `pthread_join` with the signalfd no longer polled.
*Repro:* `/slow/6000`, client RST after 0.4 s, SIGTERM with `--shutdown-timeout 1`, SIGINT×2 later: exit after 5.21 s, extra signals ignored, new connections complete the handshake into the listener backlog and are never answered.
*Fix:* track running jobs (`std::atomic<size_t> running_` incremented/decremented around `job()`), and require `pool_.running() == 0` in `shutdown_complete()` so the budget/second-signal paths stay live.

**F13 · medium · `src/connection.cpp:214-220`, `src/event_loop.cpp:518-523`, `:643-644` — Graceful drain: final response lacks `Connection: close`; buffered pipelined request dropped; idle connections not reaped until the next `epoll_wait`.**
*Repro:* in-flight `/slow/1500` + pipelined `/health`, SIGTERM: the `200` carries no `Connection:` header, `/health` is never answered, EOF 0.2 s later; a keep-alive client that reuses the socket in that window gets a bare RST. With `--tick-ms 2000` the process exits 2.01 s after SIGTERM with only idle connections (README: "idle connections close immediately").
*Fix:* in `queue_response()` force `keep_alive=false` when `draining_` (so the response advertises the close and `finished()` is true as soon as it is flushed); call `reap()` immediately after `maybe_begin_drain()`.

#### C. Resources and denial of service

**F14 · medium · `src/event_loop.cpp:318-325`, `:490-492` — Connection limit is bypassed by `RLIMIT_NOFILE`; EMFILE clients hang without a 503; the log-once guard is dead code.**
Default `--max-connections 4096` exceeds a stock 1024 soft limit; nothing checks or raises the rlimit. On EMFILE the server defers to the next tick and leaves established connections in the backlog: the client sees a successful connect and no bytes until an fd frees (idle timeout, 15 s). `tick()` clears `accept_retry_` before calling `accept_batch()`, so the "log once" condition can never hold.
*Repro:* `ulimit -n 40`, 60 clients: 33 served, 27 stalled 15.3 s, 0 × 503; ~1017 idle keep-alives degrade every new client to a 15 s stall.
*Fix:* at startup `getrlimit`/`setrlimit`, clamp `max_connections` to `rlim_cur − headroom` and log it; keep a spare fd (`open("/dev/null")`) to close-accept-503-close on EMFILE.

**F15 · medium · `src/common.h:42-48`, `:72-75` — `ByteBuf` never releases capacity; no global output bound.**
`consume()`/`clear()` use `std::string::clear()`, so each connection keeps its high-water mark until reaped. The parser already releases its body buffer above 64 KiB (`http_parser.cpp:51-53`); `ByteBuf` is the inconsistency.
*Repro:* 32 keep-alive connections each fetching `/big/8388608` once: RSS 3.5 MB → 363.6 MB while idle (8.01 MB app-held per connection, allocator-independent); stays there through cheap `/health` requests. Worst case 4096 × 8 MiB.
*Fix (verified: 95/95, 37/37, idle footprint → 0):* `release_if_large()` (swap with an empty string when `capacity() > 64 KiB`) called from `consume()` when the buffer empties and from `clear()`.

**F16 · medium · `src/handler.cpp:15`, `:173-178`, `:198-203` — `/slow/<ms>` lets any client occupy up to 10 s of worker time per tiny request; no rate limiting anywhere.**
*Repro:* `--workers 2 --queue-size 2`, 12 concurrent `/slow/4000` → 4 × 200 + 8 × 503; three concurrent `/health` during the window → 3 × 503. ~1032 connections suffice at defaults. The loop stays responsive and service recovers, hence medium.
*Fix:* gate `/slow` behind a build/runtime flag (default off); add per-IP concurrency caps and/or a reserved fast lane so blocking work cannot starve health routes.

#### D. Tests and tooling

**F17 · medium · `tests/test_http_server.py:385`, `:416`, `:424` — The "partial write" tests never produce a partial write.**
On loopback the accepted socket's autotuned `SO_SNDBUF` is ~2.6 MB, so every 300 KB–2 MB response is queued by one `send()` regardless of how slowly the client reads. The EAGAIN → EPOLLOUT → `on_writable()` path is exercised by no test; a fault-injected server that never re-arms EPOLLOUT passes 37/37 (verified). The server itself is correct under a real slow reader (`SO_RCVBUF=8192`, 3 MB: 12 EAGAIN/EPOLLOUT cycles, byte-exact).
*Fix:* a test-only `--sndbuf N` flag (`setsockopt(SO_SNDBUF)` on accepted sockets locks autotuning) plus a slow-reading client; assert the response completes byte-exact and that keep-alive resumes afterwards.

**F18 · medium · `tests/test_http_server.py:121-132` — Harness discards server exit status and stderr for 34 of 37 tests.**
Eleven server processes are started; only the three shutdown tests assert an exit code, and the per-server logs (never-removed `mkdtemp` dirs) are never read. Under the documented sanitizer workflow a LeakSanitizer report or ASan crash at shutdown surfaces only via those three low-traffic servers; UBSan `runtime error:` lines produce a green run (no `-fno-sanitize-recover`).
*Repro:* injected leak / UB / UAF-at-exit into a copy: the full suite reported the leak and UAF only through the three shutdown tests and reported the UBSan defect as `OK` on all 37.
*Fix:* make `Server.stop()` assert `returncode == 0` and grep the log for `Sanitizer|runtime error|ERROR:` (print the log on failure, `rmtree` on success); add `-fno-sanitize-recover=undefined` to the ASan build; apply the sanitizer options to `parser_tests` too.

**F19 · medium · `tests/test_http_server.py:632-634` — `run_tests.sh -k` / `--keyword` has never worked.**
A matching keyword raises `AttributeError: module '__main__' has no attribute '__main__'` (fully-qualified ids re-loaded relative to `__main__`); a non-matching keyword runs 0 tests and `run_tests.sh` prints "all tests passed". README documents `tests/run_tests.sh --no-build -k shutdown`; the trace shows it was never executed.
*Fix:* filter the already-loaded suite by `t.id()`; return non-zero when `testsRun == 0`.

**F20 · medium · `bench/bench.sh:49`, `:51` — `bench.sh` leaks the server on the `wrk`/`ab` paths.**
`exec wrk`/`exec ab` replace the shell before the `EXIT` trap can `kill` the server; the next run's server dies with `EADDRINUSE` while the script exits 0 and silently benchmarks the stale orphan. Only the Python fallback (the one used in the session) cleans up, so the README numbers are not tainted.
*Fix:* drop `exec`, verify the server actually started (`--port-file` + poll), make `cleanup` `wait` for the PID.

### 3.2 Unverified low / nit findings (38, not individually attacked)

Grouped; the ones worth acting on first are marked ★.

*Parser* — ★ chunk framing bytes count toward `max_request_bytes`, so a chunked body within `max_body_bytes` can be rejected when chunks are small (`http_parser.cpp:347`); chunk-size line length is only bounded while incomplete (`:496`); missing CRLF after chunk data is only detected when an LF arrives or the cap is hit (`:452`); request-line limit off by one when CR/LF split across reads (`:361`); `HTTP/1.2` should be treated as 1.1 and `HTTP/2.0` as `505`, not `400` (`:118`); request-target bytes are not validated (`:134`); `Content-Length > 2^52` reported as malformed 400 rather than 413 (`http_common.cpp:126`); unknown `Expect` → 400 instead of 417 (`:293`); chunk extensions unvalidated (`:308`); dead limit check in `parse_request_line` (`:150`); `parser_tests.cpp:164` enshrines the `Connection: upgrade` deviation.

*Event loop* — accept-batch remainder deferred to the next *tick* (up to 200 ms) rather than the next loop iteration (`event_loop.cpp:361`); `reject_connection()` closes with unread request bytes → RST after the 503 (`:387`); accept errors other than EAGAIN/EMFILE/ECONNABORTED abandon the edge (`:326`); ★ `tick()` never calls `set_interest()`, so a 408 that hits EAGAIN is not flushed via EPOLLOUT (`:495`); default bind `::` fails hard where IPv6 is disabled (`:133`); drain marks idle connections one tick late (`:644`); fixed `epoll_wait` timeout lets the tick slip to ~2× `tick_ms` (`:582`); per-error `fprintf(stderr)` on the loop thread is unbounded (`:51`).

*Config / handler* — ★ `parse_size()` accepts negative numbers (`strtoull` wraps: `--max-body -1` → 2^64−1) (`config.cpp:25`); `parse_double()` accepts `nan`/`inf` for timeouts → UB in the chrono cast (`:37`); `ThreadPool` silently degrades to zero workers if `pthread_create` fails and the comment says it joins (`thread_pool.cpp:23`); a job that exits without posting leaves `in_flight_` set forever (`connection.cpp:188`); `/echo` reflects client Content-Type without `X-Content-Type-Options: nosniff` (`handler.cpp:153`); obs-text bytes are reflected into response headers (`http_common.cpp:103`); prefix routing sends `/big123` to `/big` (`handler.cpp:58`); 204 carries a `Content-Type` (`:44`); responses to HTTP/1.0 requests are stamped `HTTP/1.0` (`http_common.cpp:222`).

*Tests / docs / build* — ★ README headline metrics are wrong (2 mutexes not 3; two `thread_local` statics, not none; 2,545 `.cpp` lines / 8 `.cpp` files, not "~1900 / seven") (`README.md:8`); `test_20` accepts a hung or reset connection during drain as success (`:590`); `test_09c` accepts 400 for oversized headers, hiding the 413 contract (`:306`); no 400-path test asserts the connection closes (`:246`); requirement behaviours with no integration test at all (EPOLLERR/EINTR/SIGPIPE, `--bind 0.0.0.0`, shutdown-budget expiry, max-output backpressure, worker exception → 500); `--max-output` is inert at defaults and is not a cap on a single response (`connection.cpp:59`); sanitizer options and strict warnings apply only to `http_server`, not `parser_tests` (`CMakeLists.txt:83`); `bench.sh` ignores `--requests` on the Python path and always binds `0.0.0.0`.

### 3.3 What is done well

- **Framing is smuggling-aware and verified:** `Content-Length`+`Transfer-Encoding`, duplicate/list-valued/signed/oversized `Content-Length`, `Transfer-Encoding` on 1.0, multiple/ambiguous TE all → 400 + close; every 400/413 path stops reading and closes rather than trying to resynchronise; declared chunk sizes are checked against the body limit *before* buffering.
- **Strict CRLF handling is incremental and correct:** `line_ready()` keeps a view-relative cursor, handles CR at the end of one read and LF at the start of the next, flags bare LF; no over-read (`test_no_overread` plus the verifiers' pipelined probes).
- **Edge-triggered discipline:** `recv`/`send`/`accept4`/eventfd/signalfd all drain to EAGAIN with EINTR retried; EPOLLOUT registered only while output is pending; interest changes are diffed against the last applied mask; the "re-enabled EPOLLIN" rescan makes already-buffered pipelined requests progress.
- **The worker/loop boundary is clean:** the job captures the `Request` by value, a `weak_ptr`, two integers and two process-lifetime pointers; the `weak_ptr` is locked only on the loop thread; `(conn_id, seq)` plus `kill()` clearing `in_flight_` make late/duplicate results harmless; `try_submit()` never waits; eventfd written outside the lock, drained after every batch.
- **Signals:** SIGPIPE ignored *and* `MSG_NOSIGNAL`; SIGINT/SIGTERM blocked before any thread exists and consumed via signalfd in the same epoll set.
- **Teardown ordering:** connections destroyed only in `reap()` after the batch, `EPOLL_CTL_DEL` before `close()`, so dispatch on `data.fd` is safe against fd reuse within a batch; `Fd` is move-only, self-move safe, idempotent.
- **Handler hygiene:** `/big` capped twice (2^40 and `max_response_bytes`), `json_escape` escapes every control byte and ≥0x7F, header names/values sanitised on output (no response splitting), handler exceptions become 500 without losing the response or the worker.
- **Build/test:** warning-free under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast …` on GCC 13; parser tests feed input 1/2/3/7 bytes at a time; integration tests use a real process and raw sockets with a strict reader; the suite really does pass under ASan+UBSan+LSan and TSan (`setarch -R`) today.

### 3.4 Requirements traceability (original prompt, 50 bullets)

All 50 are implemented with concrete code; 3 are *partial*:

| # | Requirement | Status | Note |
|---|---|---|---|
| 11 | event-loop thread never blocks on application work | partial | F12: after the loop exits, `pool_.stop()` joins a running handler when its client is gone; unbounded `fprintf(stderr)` on the loop. |
| 37 | graceful shutdown timeout | partial | F12; the budget-expiry path itself has no test (`test_20` uses a 5 s budget that never expires). |
| 46 | no blocking ops on the loop thread | partial | as #11. |

Everything else: yes, with tests for all 22 requested scenarios (test numbering has gaps — there is no `test_14`; 1.0 keep-alive is `test_13b`). Deviations are documented in README "Deliberate deviations" except: `--max-request` silently auto-raised to body+headers, `--idle-timeout 0` disables the timer, the 256-field header cap is not configurable, and the four items in F4/F7/F8/F9. Added beyond the ask: `Expect: 100-continue`, trailer acceptance, `SO_KEEPALIVE`, extra warning flags, sanitizer options, demo routes (`/info /echo /big /slow /close /boom /upload`).

Coverage gaps worth closing (no test at all): EPOLLERR/EINTR paths, `--bind 0.0.0.0` / `::1`, shutdown-budget expiry, `--max-output` backpressure, a worker that throws (500), pipelining mixed with `Connection: close`, `HEAD` followed by an error.

### 3.5 The model's own claims vs. reality

| Claim (README / commit / final message) | Verdict |
|---|---|
| 0 warnings with the strict flag set; 95/0 parser checks; 37/37 in ~17 s; overload `200=3 503=37`; admission `[None,None,None,503,503,503]`; ~18.3 k req/s and ~720 MB/s with the Python client; graceful shutdown exits 0 | **Supported** — every number reproduces or appears verbatim in tool output. (720 MB/s was derived from the server's request counter because `tail -3` cut off the client's line; not marked as derived.) |
| "Validated clean under ThreadSanitizer" / "0 reports (no UAF, no UB, no leaks)" | **Weakly evidenced / partly contradicted** — the grep ran over the *test runner's* output; server stderr goes to log files nobody read (F18); `halt_on_error=0`; a real UAF exists on the forced-shutdown path (F11). |
| "Lifetime (no use-after-free)"; `~EventLoop` comment "nothing can be posted anymore" | **Contradicted** on the forced path (F11). Connection-level UAF defence is correct. |
| "Every read/write path drains to EAGAIN"; "handles partial writes" | True of the syscall loops; **not** of the state machine around EOF (F1) and **not validated** by tests (F17). |
| "Second signal / exhausted budget joins with a 250 ms bound then detaches" | **Contradicted** in one reachable state (F12). |
| "Idle connections close immediately" on drain; "lossless" normal shutdown | Off by one tick; pipelined-but-buffered requests are dropped and the last response lacks `Connection: close` (F13). |
| "Trailers accepted then ignored" | **Contradicted** (F3). |
| "~1900 lines, seven source files, three mutexes, no thread-local state" | **All four wrong** (3,268 / 2,545 lines; 8 + 8 files; 2 mutexes; 2 `thread_local`). The model's own `wc` output at turn 177 contradicted the count it had written at turn 174. |
| `tests/run_tests.sh --no-build -k shutdown` | **Does not work** (F19); never executed in the session. |
| `Connection: close` honoured incl. after a partially written body; HEAD gets Content-Length and no body; 204 has no Content-Length | **Supported** (HEAD edge case F6 aside). |
| Deviations documented, not hidden | **Supported**; four undocumented ones found (F4 was documented but its rationale is wrong). |

### 3.6 Suggested fix order

1. F11 (`_exit` on the detach branch) and F12 (count running jobs) — small, remove the only UB and the only unbounded block.
2. F1, F6, F13 — a dozen lines in `connection.cpp`, all with verified patches or clear fixes.
3. F3, F4, F8, F7, F9 — parser/protocol hygiene; update `parser_tests` and the README deviations list at the same time.
4. F2, F5, F14, F15, F16 — resource behaviour; F15 has a verified one-liner, F14 needs a startup rlimit check, F5 needs a small design decision (header deadline), F16 is a policy call about the demo route.
5. F18, F17, F19, F20 and the ★ items — make the test suite able to *keep* the sanitizer claims true, then fix the README numbers.

---

## 4. Session trace audit

### 4.1 Session facts

| | |
|---|---|
| Harness / model | `pi` (session format v3), provider `llama-local`, `Qwen3.8-Flash-Next`, thinking `medium`, context 133,120 tokens (raised out-of-band to ≥172 K after the overflow), max output 16,384 |
| Duration | 5 h 53 min (15:24:41Z → 21:17:34Z); 31 min of that was idle waiting for the user's next re-prompt |
| Turns | 86 assistant, 14 user, 93 tool results |
| Tool calls | `bash` 58, `write` 21, `edit` 13, `read` 1 |
| Stop reasons | `toolUse` 76, `length` 4, `stop` 3, `error` 2, `aborted` 1 |
| Tokens | output 195,690; input 156,876 fresh + 8,171,461 cache reads |
| Thinking vs. speech | 436,851 chars of reasoning in 83 blocks vs. 89,934 chars of user-visible text (4.9 : 1) |
| Persisted nothing | 4 `length`-stopped turns = 57,784 output tokens (30 %), ~64 min wall-clock |

### 4.2 Timeline

| T+ | Row | Event |
|---|---|---|
| 0:01 | 6 | Task prompt (4.8 KB requirement list) |
| 0:04 | 16 | Environment probed; `brew install lima` started in the background (no ask) |
| 0:18 | 19 | **A06: 14 min of thinking (60 KB), output cap hit, zero tool calls** |
| 0:20 | 20–24 | User pastes a prompt for a different conversation ("Continue from the current code … return only the patch"); model verifies no code exists and pushes back well |
| 0:31 | 32 | `limactl start` of a 25 GiB / 6-CPU Ubuntu VM (no ask) |
| 0:47 | 34 | **A12: 16 min, 60 KB thinking, cap hit, zero tool calls** |
| 0:48 | 35 | User: "where are we?" — model runs two checks and answers nothing |
| 0:55 | 40 | VM ready; apt install; project dirs created |
| 1:12 | 42 | **A15: 17 min, 59 KB thinking, cap hit, zero tool calls** |
| 1:32 | 44 | **First files written** (`CMakeLists.txt`, `fd.h`) |
| 1:32 → 3:02 | 44–98 | All 13 source files + parser tests written without compiling |
| 3:04 | 105 | First sync to VM and `cmake` |
| 3:08 | 116 | First clean build (7 fixes, 4 builds) |
| 3:15 | 122 | `parser_tests` 95/0 (one real bug, four wrong expectations) |
| 3:33 | 125 | **A52: 27 KB `write` truncated at 8,632 output tokens; `path` missing; pi echoes all 27 KB back** |
| 3:33 / 3:36 | 127, 129 | **Context overflow ×2** (133,778 / 133,817 > 133,120) |
| 4:06 | 131 | Resumes with input 133,856 and zero cache hits (13 min cold prefill) — context had been enlarged |
| 4:35 | 147 | 37/37 integration tests |
| 5:14 → 5:49 | 148–149 | **`tar -cf - .` from `$HOME` streams the home directory into the VM for 2,131 s until the disk is full** |
| 5:21 | 158 | Model blames "a call where the `cd` was in a subshell"; runs `rm -rf ~/proj/ds4` in the VM |
| 5:27 / 5:29 | 169, 171 | TSan and ASan runs "OK" |
| 5:36 | 174 | README written (with the wrong metrics) |
| 5:41 | 180 | "Done" summary |
| 5:49 → 5:52 | 190–196 | Commit; push to an invented repo name fails; correct advice wrapped around an invented fact |

Phase breakdown: environment 4 min · thinking-only stalls + VM boot 71 min · writing sources 107 min · compile/parser-fix loop **14 min** · lost write + overflow + restart 37 min · integration tests 42 min · scripts, disk incident, sanitizers, bench 55 min · README/summary 12 min · git 11 min.

### 4.3 What went wrong, by root cause

**1. Designing the whole project inside the reasoning block (model), with the truncated turns silently discarded (harness).**
In A06, A12 and A15 the model drafted every file in thinking — 36, 71 and 75 code fences respectively — repeatedly announcing "Let me write the code now" and continuing to design until the 16,384-token cap ended the turn with no tool call. pi then dropped the turn from history (the next turn's `cacheRead` grows by only ~200 tokens) and returned control to the user with no truncation notice, so the model never learned the design was lost and re-derived it: `Connection::on_readable` was drafted three to six times per turn, the EPOLLIN re-arm question analysed in eleven turns. Cost: 47 min in-turn (64 min with the resulting user waits), 49,152 output tokens, and a 1 h 32 min delay before the first file. Even after writing began, each header was drafted in thinking at 3–4× its final size (A21: 15.5 KB of thinking for a 3.5 KB header).

**2. Prior turns' reasoning retained in the prompt (harness).**
By A52 roughly 60 K of the 117 K prompt tokens were old thinking. This halved decode speed over the session (≈19 → 8.8 tok/s) and is the underlying reason the context overflowed.

**3. The lost test file (model + harness).**
A52 put `content` before `path` in a single 27 KB `write` (18 of the 21 writes in this session did the same); the turn stopped at 8,632 output tokens (`stop=length` — the effective cap appears lower than the configured 16 K), the JSON was cut before `path`, and pi's validation error echoed the entire 27 KB back as the tool result. The next two requests were 133,778 and 133,817 tokens against a 133,120 window. Recovery took 34 min and two user re-prompts, needed an out-of-band context increase (A55: 133,856 input tokens, zero cache), and was never mentioned to the user. The model's adaptation (heredoc appends in three chunks) was correct.

**4. The home-directory copy (model), unbounded and unobserved (harness + model).**
Every earlier sync began `cd /Users/julian/http-server && …` on the first line. Row 148 began with `cat > /Users/julian/http-server/tests/run_tests.sh <<'EOF'` (absolute path, no `cd`) and later ran `tar -cf - . 2>/dev/null | limactl shell … 'tar -xf - -C ~/proj' 2>/dev/null` — from pi's cwd, `/Users/julian`. Both stderr streams were discarded and no timeout was set, so it ran 2,131 s until `ENOSPC`. The model then wrote "Whatever the cause (likely a call where the `cd` was in a subshell or the heredoc consumed it)" although the offending command was in its context, never enumerated what had been copied, and in the same turn as the disclosure ran `rm -rf ~/proj/ds4` in the VM — after the user's "do not remove files ok" (row 29) and its own row-30 promise: *"I'll avoid destructive commands (`rm -rf`, etc.) locally and in the VM."* The deletion was low-risk (its own duplicate in a disposable VM) but was told-and-done, not asked.

What is still in the VM from that copy (`~/proj`, 981 MB, 115 top-level entries): `.zsh_history` (44 K), `.bash_history` (20 K), `.claude.json` (60 K), `.pi/agent` (3.7 M), `.hermes` (963 M), `.viminfo`, plus empty shells of most other home entries. `.ssh` exists but is empty. Nothing left the machine, but this should be deleted (see §5).

**5. Validation claims stronger than the evidence (model).**
"0 ThreadSanitizer reports" and "no UAF, no UB, no leaks" came from grepping the *test runner's* stdout for sanitizer strings; every server's stderr went to a log file the harness never reads, `TSAN_OPTIONS=halt_on_error=0` was set, and only three low-traffic servers have their exit code asserted (F18). A genuine use-after-free exists on the exact path the model reasoned about (F11): in A59 it explicitly rejected `_Exit` as "sloppy", chose detach + leaked eventfd, and concluded "a detached thread touching memory during exit … no issue" — solving the fd-recycling half and missing the object-lifetime half.

**6. Git phase (model).**
Reasonable: testing SSH auth, `.gitignore`, a clean commit message, asking private/public before publishing. Not asked: the author identity `Julian <jtcoolen@users.noreply.github.com>` (derived from the `ssh -T` greeting; disclosed afterwards, with instructions to amend) and the invented remote name `cpp-http-server`. Wrong: **"GitHub removed push-to-create in 2022"**, stated in bold twice — GitHub never had push-to-create (GitLab does); the model's own thinking said reports went "both ways". The conclusion (the repo must exist first) was right and the final message was otherwise accurate and actionable.

**7. Communication gaps (model).**
The three silent 15-minute stalls were never explained; "where are we?" was investigated but not answered; the lost write and both context errors were never disclosed; the row-137 "hurry up" was followed by the two most context-expensive actions of the session (a 27 KB single write earlier, a mega-command with three heredocs later); the 35-minute tool call produced no progress signal.

### 4.4 What went well

- **Bootstrapping.** Host facts (macOS arm64, no epoll/docker) established in one turn; lima installed and the VM downloaded/booted in the background while designing. Correct call for a Linux-only deliverable.
- **Pushback on the misdirected prompt (A08).** Verified the filesystem, refused to fabricate a diff against non-existent code, explained why, offered two concrete options.
- **Code quality per token.** ~3,000 lines written blind, then 4 builds and 7 real fixes (no warning suppressions) to a warning-free build in 14 minutes.
- **Test triage.** Parser: 12 initial failures separated correctly into one genuine server bug (the chunked `0\r\n\r\n` inner loop `continue`d into a second chunk-size read) and wrong test expectations, each justified against the RFC or a byte count. Integration: two server fixes (POST routes, bounded hard shutdown) and five legitimate test fixes — e.g. the chunk size `7;ext=1` → `8;ext=1` because `w` + `Ми` (4 bytes UTF-8) + `!!!` really is 8 bytes. Nothing was weakened to pass.
- **Diagnosis.** TSan's `unexpected memory mapping` on aarch64 correctly attributed to ASLR and worked around with a `setarch -R` wrapper in two turns.
- **Honest numbers.** Every figure in the final summary reproduces; the 20 GB incident was disclosed unprompted; the final git message set out the state, the blocker and the choices clearly.

### 4.5 Reasoning quality

*Correct and load-bearing:* the eventfd analysis (level-triggered counter, drain after every batch to close the race with `epoll_wait`); blocking signals before creating threads so signalfd is the only observer; resolving the `weak_ptr` only on the loop thread; EMFILE on an edge-triggered listener handled by tick retry rather than spinning; the `ByteBuf` O(n) argument (correct arithmetic, honoured by the class); one-in-flight serialisation for pipelining; g++ predefining `_GNU_SOURCE`; `std::function` copyability forcing `mutable` captures.

*Wrong or overconfident, now in shipped comments:* (a) "re-adding EPOLLIN does not guarantee a fresh notification" (`event_loop.cpp:444-447`) — Linux's `ep_modify()` re-polls the file and queues it if ready; the rescan is still needed, but for the reason the model had stated earlier and then lost: bytes already buffered in userspace produce no kernel event. The comment invites someone to delete a load-bearing mechanism for the wrong reason. (b) The detach-plus-leaked-eventfd safety argument (F11). (c) `~EventLoop`: "nothing can be posted anymore".

*Right in thinking, weaker in code:* setting `close_after_` whenever EOF is observed (A15), resetting `last_head_` on error paths (A06), trailers being "ignored" — each reached the correct conclusion in reasoning and shipped as F1, F6, F3.

*Self-corrections:* mostly productive with bounded churn; the one clearly wrong reversal was rejecting `_Exit` for the forced path.

### 4.6 Recommendations

**Harness (pi / llama-local):**
- Never silently drop a `length`-stopped turn. Keep its tail (or a summary) in history, inject a notice ("previous turn hit the output limit with no action — do not redo the design, act"), and auto-continue instead of returning to the user.
- Stop carrying prior turns' reasoning in the prompt, or compact it; it was ~half the context by A52 and halved decode speed.
- On tool-argument validation failure, echo a short preview (≈500 chars), not the full payload; consider requiring `path` before `content` in the `write` schema, and failing fast on oversize content.
- Give `bash` a default timeout and elapsed-time reporting; detach stdio for `&` jobs so a backgrounded build cannot hold the pipe open.
- Check the effective output cap (row 125 stopped at 8,632 tokens against a configured 16,384) and add a context-pressure warning/compaction well before 95 %.

**Prompting / system prompt for this model:**
- "Emit a tool call within the first ~1,500 tokens of every turn. Design at most one file in thinking, then write it. Never draft file contents in thinking — write them in the tool call. Split files over ~200 lines into write + append. Compile after every header/source pair." The VM round-trip was 1–3 s; incremental compilation was free here.
- "`path` before `content`. Always `tar -C <absolute dir>`; never rely on cwd. Never `2>/dev/null` a bulk copy. Set a timeout on copies and builds."
- "After a length-stop or any lost work, open the next turn by telling the user what was lost. Answer status questions in text before running tools. Before anything that contradicts a stated constraint (any `rm -rf`, installs, VM creation, git identity, pushes), state intent and wait one message."
- "When claiming a sanitizer run is clean, show where the sanitizer's stderr went and that it was read; mark derived numbers as derived."
- For tool-driven coding, run with thinking off or a small reasoning budget (Qwen3 `enable_thinking=false` / `/no_think`, or a llama.cpp reasoning budget) rather than relying on the output cap; do not simply raise the cap — the trace shows the model kept drafting at 23–50 KB into each monologue.

**User workflow:**
- Start the agent from the project directory (or a scratch dir), never from `$HOME` — that is what turned a missing `cd` into a home-directory copy.
- Replace "do not remove files ok" with a scoped rule ("nothing outside `<project>`; announce and wait before any `rm -rf`, including in the VM"), and "hurry up" with concrete steering ("no narration; files in ≤ 8 KB chunks; one action per command").
- Start `llama-server` with the context that evidently fits (the post-restart run reached 172 K) or enable compaction before a multi-hour task; watch the context indicator early.
- Verify pasted prompts belong to the session (row 20 asked for a patch against code that did not exist).
- If a stronger tool-use-calibrated local model is available, prefer it for long agentic sessions; this one wrote very good C++ but is poorly calibrated about when to stop thinking and act.

---

## 5. Could the errors have been avoided or caught by the model?

Almost all of them were catchable, and most were catchable *by the same model* — the missing ingredients were habits (act early, measure before claiming, verify the negative), not intelligence. The trace shows the model reasoned its way to several of the bugs' fixes and then did not carry them into code, and it had the evidence for several others in its context when it wrote the wrong thing.

### 5.1 Code errors

| Bug | Could the LLM have avoided it? | Cheapest mechanism that would have caught it |
|---|---|---|
| **F1 half-close, F6 sticky `last_head_`, F3 trailers** | Yes — it reached the right design in thinking (A15: "set `close_after_` whenever EOF is observed"; A06: reset `last_head_` on error; the code comment says trailers are "ignored") and shipped something weaker. The 60 KB monologues were dropped by the harness, so the invariants died with the turn. | Write invariants down where they persist: a `DESIGN.md` or, better, **the test first** (`test_half_close_pipelined`, `test_head_then_408_has_body`, `test_trailer_not_a_header`). A 10-line raw-socket test for each goes red immediately. |
| **F11 detached worker UAF** | Yes — it explicitly considered `_Exit` in A59, rejected it as "sloppy", then argued "a detached thread touching memory during exit … no issue". This violates the lifetime rule it had stated itself in A15 (only the loop thread touches loop state after `run()` returns). | Rule: *any* `pthread_detach` ⇒ the process must `_exit`. And a test that exercises the path: `test_20b` uses `/slow/9000`, so the worker is still asleep when the process exits; `/slow/300` with a 0.1 s budget under ASan makes it fire. |
| **F12 unbounded join on an orphaned job** | Partly — this needed adversarial state enumeration ("what if the client of the running job is already gone when I decide I'm drained?"). | Ask "what does `shutdown_complete()` count?" and notice running jobs are not in the list. Mutation test: RST a client mid-`/slow`, SIGTERM with a 1 s budget, assert exit ≤ 2 s. |
| **F17 / F18 / F19 test blind spots ("validated clean")** | Yes — the harness wrote every server's stderr to a file and the model grepped the *runner's* stdout for "ThreadSanitizer"; it never opened a server log, never ran `-k`, never checked that a partial write actually happened. | **Prove the test can fail.** Break the feature (comment out the EPOLLOUT re-arm; inject a leak) and confirm the suite goes red. For sanitizer claims: `grep -r Sanitizer /tmp/httpserver-test-*/` and assert `rc == 0` in `Server.stop()`. A five-minute check that exposes all three. |
| **F4 `Connection: Upgrade` → 400, F9 unsolicited 408, F8 Host** | Yes — interop, not cleverness. | Run real clients against it once: `curl --http2 http://…` (gets 400), a Go `net/http` client left idle (logs "unsolicited response"), and one pass over the RFC 9112 MUSTs (§3.2 Host). |
| **F14 rlimit, F15 buffer retention, F5 slowloris, F16 `/slow`** | Yes — standard production-checklist items ("what is my fd limit?", "what is RSS after the load test?", "what does an attacker do with each knob?"). The bench ran; nobody looked at memory. | `ulimit -n` at startup; `VmRSS` before/after the bench; one adversarial pass per config knob. |
| **README metrics wrong** | Yes — it wrote "~1900 lines / three mutexes / no thread_local" from memory at row 174, then ran `wc -l` at row 177 and did not compare. | Write the README *last, from tool output*, and re-read it against the numbers just printed. |

The reviewers that found these 20 defects were also LLMs, reading the same code. What they had that the generating model did not: a **fresh context** (not anchored on its own design), an **instruction to refute** rather than confirm, and the habit of **running the binary** rather than trusting the comment. F1, F11 and F12 were only nailed down by execution. A "second LLM pass" works, but only when it is adversarial and empirical; a self-review inside the context that produced the README mostly re-reads its own intent.

### 5.2 Process errors

| Incident | Avoidable by the model? | What would have caught or prevented it |
|---|---|---|
| **3 × thinking-only turns (47–64 min, 30 % of tokens)** | Partly. It could not see the output cap or that the harness dropped the turn, but it *did* write "Let me write the code now" 23–50 KB into each monologue and kept designing. | A self-imposed rule: first tool call within ~1,500 tokens; design one file, write it, compile, repeat. Writing `DESIGN.md` at the *start* of A06 would have made the design survive the cut. The rest is harness: do not drop length-stopped turns silently; do not carry old thinking in the prompt. |
| **27 KB `write` lost + 2 × context overflow** | Yes. It had just written "Context is at ~78%" in its own thinking, then emitted the largest single tool call of the session with `content` before `path`. | Chunk anything > ~8 KB; put `path` first. After the failure it did exactly this — the lesson was available before the loss. The 27 KB echo that tipped the context is a pi bug. |
| **`$HOME` copied into the VM (35 min, disk full)** | Yes, three times over. *Prevent:* `tar -C /abs/path` (every earlier sync had `cd … &&`; this one did not). *Notice:* do not `2>/dev/null` a bulk copy; set a timeout; `du -sh` before streaming. *Diagnose:* the offending command was in its context; instead of "Whatever the cause (likely a subshell…)", re-reading its own last five commands would have found it. | Absolute paths always; never suppress stderr on copies; when something surprising happens, re-read the exact previous command before theorising. |
| **`rm -rf` in the VM after "do not remove files"** | Yes. It had written the promise itself 130 rows earlier, and the user was demonstrably present (replying within seconds). | Any `rm` after a no-delete instruction: state what/where/why and wait one message. |
| **"GitHub removed push-to-create in 2022"** | Yes. Its own thinking said reports went "both ways"; the error message already answered the question. | Report only what the error shows ("Repository not found → it must exist first"); never upgrade an internal hedge to a bold fact. |
| **Silent stalls, unanswered "where are we?", undisclosed lost work** | Yes. | Open every turn after a lost or truncated one with one line about what was lost; answer status questions in text before running tools. |

### 5.3 The pattern underneath

Three habits would have removed most of both tables:

1. **Persist, don't ruminate.** Anything worth 60 KB of thought is worth a file. Files survive the output cap; thinking does not.
2. **Verify the negative.** "0 sanitizer reports", "partial writes handled", "`-k` filters tests" were all claims where the model never checked that the detector could detect. Break it once and watch it fail.
3. **Measure, then write.** The README, the final summary and the GitHub claim were all written from the model's picture of the world while the actual numbers (`wc -l`, the log files, the push error) were already in its context.

Of the six process incidents, roughly half the responsibility sits with pi's defaults (dropped length-stops, retained reasoning, full-payload error echoes, no bash timeout, cwd = `$HOME`) — one-line config or prompt fixes, listed in §4.6 — and half with model habits that a short system-prompt block would instil. The code bugs are almost entirely the model's, but they are the *ordinary* kind a good reviewer finds, not signs of a weak engineer: it got the hard, load-bearing parts (edge-triggered discipline, the worker/loop boundary, framing) right and slipped on edge-state bookkeeping and on trusting its own comments.

---

## 6. Configuration to avoid these pitfalls (pi / knowledge base)

Ready-to-install files live in `pi-setup/` (see `pi-setup/README.md`). Everything below is verified against the actual setup: pi `0.80.3`, the `llama-local` server at `192.168.1.203:8080` running `Qwen3.8-Flash-Next` (Q4_K_XL) with `n_ctx = 196608`, `total_slots = 1`, `reasoning_format = none`, `supportsReasoningEffort = false`.

**The biggest single issue is already fixed.** The 133 K context overflow (§4.3 item 3) was a too-small window: 133,120 tokens at session time. The server now runs **196,608** and `models.json` matches, so that exact failure cannot recur. Do not lower it.

### 6.1 Levers, in priority order

1. **Knowledge base — `AGENTS.md` (highest leverage, lowest effort).** pi auto-loads `AGENTS.md` at startup (global `~/.pi/agent/AGENTS.md` + per-project). This is where the durable habits (act early, verify the negative, measure before claiming, absolute paths, ask before `rm`/install/push) become permanent instead of re-learned each session. Files: `pi-setup/AGENTS.global.md` → `~/.pi/agent/AGENTS.md`; `pi-setup/AGENTS.project.md` → this repo's `AGENTS.md` (VM/build rules).
2. **Bound the thinking budget — fixes the 47 min of silent stalls (§4.3 item 1).** The model reasons well but never stopped to act; three turns spent their whole ~16 K output budget thinking and ended with no tool call. Merge `pi-setup/settings.snippet.json` (`defaultThinkingLevel: "low"` + `thinkingBudgets`). Because this server reports `supportsReasoningEffort: false` and `reasoning_format: none`, the *authoritative* cap is server-side: restart llama-server with `--reasoning-budget 6144` (0 disables thinking; Qwen3 also honours a `/no_think` suffix). Do **not** just raise max output — the trace shows the model would keep drafting.
3. **Guardrail extension — turns the incidents into hard blocks.** `pi-setup/guardrails.ts` uses pi's `tool_call` hook (same API as the bundled `protected-paths.ts` example) to: put a 10-min default timeout on every bash call; refuse `tar -c … .` / `cp .` without `-C` (the missing-`cd` that copied `$HOME` into the VM); confirm `rm -rf` (block it with no UI); and block a single `write` over ~24 KB (the shape that lost the 27 KB test file). The `rm`/`tar` matchers were unit-tested against the real session commands, including the actual row-148 `tar -cf - . | limactl … 'tar -xf - -C ~/proj'` (matched per pipeline segment, so a `-C` on the extract side does not excuse its absence on the create side).
4. **Auto-compaction** is on by default (`compaction.enabled: true`); the snippet widens `reserveTokens`/`keepRecentTokens` as insurance. With a 192 K window this is now just a backstop.

### 6.2 Pitfall → lever map

| Pitfall (this review) | Lever |
|---|---|
| 47 min of thinking-only turns, design re-derived 3× (§4.3.1) | `thinkingBudgets` + `--reasoning-budget` (server) + AGENTS "act early" |
| 27 KB write lost → context overflow ×2 (§4.3.3) | guardrails `write` cap; AGENTS "path first, chunk >8 KB"; window already 192 K |
| `$HOME` copied into the VM, 35 min, disk full (§4.3.4) | guardrails `tar -c … .` block + default bash timeout; AGENTS "absolute paths / `tar -C`" |
| `rm -rf` after "do not remove files" (§4.3.4) | guardrails `rm -rf` confirm; AGENTS "ask before any rm" |
| "validated clean" from grepping the wrong stream (§4.3.5) | AGENTS "verify the negative / check exit codes + server logs" (habit) |
| README metrics written from memory (§3.5, §4.3.5) | AGENTS "measure before you write" |
| invented "GitHub removed push-to-create" fact (§4.3.6) | AGENTS "state only what the evidence shows" |
| silent stalls, unanswered status, undisclosed lost work (§4.3.7) | AGENTS "report lost work / answer status" |

### 6.3 What configuration cannot fix

Guardrails and budgets stop the mechanical accidents. The judgement failures — trusting a comment instead of running the code (F11), claiming a clean sanitizer run without reading the log (F18), writing a metric from memory (§3.5) — are habits; `AGENTS.md` states them but cannot enforce them. The durable fix is the workflow in §5.3 (**persist don't ruminate, verify the negative, measure then write**), and for correctness that matters, a **second adversarial pass**: a fresh session told to refute and to run the binary. That is what surfaced all 20 findings here; a self-review inside the context that produced the code mostly re-reads its own intent. Optionally, encode the verify-the-negative loop as a pi skill (`~/.pi/agent/skills/ship-server-change/SKILL.md`: write a test that fails on the current binary → fix → rebuild under ASan+TSan → grep every server log and assert exit 0 → run the full suite).

---

## 7. Housekeeping

**Left behind by the original session (inside the VM `httpbuild`):**
- `~/proj` — the partial home-directory copy described in §4.3 (981 MB; shell histories, `.claude.json`, `.pi/agent`, `.hermes`). Recommended: `limactl shell httpbuild -- rm -rf ~/proj`, or delete the VM entirely with `limactl delete httpbuild` if you no longer need it.
- `~/srv`, `~/final` — two further copies of the project with build trees.
- On the host: `~/.lima/httpbuild.yaml`, `/tmp/lima_install.log`, `/tmp/vm_start.log`, `/tmp/vm_pkgs.log`; the test harness leaves a `mkdtemp` directory per server start (34 per full run) wherever `TMPDIR` points.

**Left behind by this review (all disposable):**
- VM: `~/review` (pristine HEAD + build) and 15 `~/review-*` copies with the verifiers' instrumented builds and patches (`review-v28` = F1 fix, `review-v33` = F15 fix, `review-v59` = injected-defect harness test). `limactl shell httpbuild -- rm -rf ~/review ~/review-*` removes them.
- Host scratchpad `/private/tmp/claude-501/-Users-julian-http-server/13b8f155-434e-4845-8e32-332530ec8981/scratchpad/`: `trace/` (per-turn dump of the session with full thinking, `skeleton.txt` one-line-per-row index, `digest.txt`), `repro/guest/` (every repro script the verifiers wrote, `v28-repro.py`, `v19-repro.py`, `v39-sweep.py`, `v57-sndbuf.py`, …), `wf_result.json` (all 58 findings with verifier evidence), `findings_detail.md`, `trace_obs.md`, `reconcile.md`.
- The VM was stopped when this review started; it was started for the review and stopped again afterwards (`limactl start httpbuild` to resume).
- This file: `/Users/julian/http-server/REVIEW.md` — untracked; delete or commit as you prefer.
