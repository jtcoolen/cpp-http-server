# FIX_PLAN — closing the findings in `REVIEW.md`

Plan of record for fixing the **20 confirmed findings (F1–F20, 15 medium / 5 low)** plus the
★-marked items from `REVIEW.md` §3.2, and for correcting the README claims that §3.5 showed to be
wrong. Nothing here is a refactor: each change is scoped to the finding it closes.

Status of this document: **proposal — awaiting the decisions D1–D8 in §7.**
Branch of record: `fix/review-findings` (one commit per F-number).

---

## 1. Ground rules

1. **Test first, and commit it red.** For every finding that is observable from the outside, write
   the raw-socket repro (Python) or parser check **before** the fix, commit it as a failing test,
   then make it green. `REVIEW.md` §5 says explicitly that F1, F6, F3, F11 and F12 were each
   *reached in reasoning and lost in code* — the only durable carrier for those invariants is a test.
2. **Verify the negative.** Each new test gets a mutation check (§5): break the mechanism the fix
   relies on, confirm the test goes red, restore. A test that cannot fail is not a fix.
3. **Read the sanitizer's output, not the runner's stdout.** F18 is the reason this is a rule:
   every server's stderr goes to a per-server log file. The gate in §2.3 greps those logs.
4. **Measure, then write.** README numbers are regenerated from `wc -l` / `grep -c` output pasted
   into the commit message, never typed from memory (§3.5: all four headline metrics were wrong).
5. **No silent behaviour changes.** Every deviation from the original 50-bullet requirement list
   gets a line in README "Deliberate deviations" (`README.md:198`) in the same commit.
6. Absolute paths in every sync/build command; `tar -C <dir>` never `tar -c .`; no `2>/dev/null` on
   a bulk copy; a timeout on builds and copies (REVIEW §4.3 item 4).

---

## 2. Verification loop

### 2.1 Why the loop is not on this host

`uname -sm` → `Darwin arm64`. The server needs `epoll`/`accept4`/`eventfd`/`signalfd`;
`CMakeLists.txt:11` hard-fails on non-Linux and `tests/run_tests.sh:38` refuses to run. The only
target is the existing lima VM.

| | |
|---|---|
| VM | `httpbuild` — Ubuntu 24.04 aarch64, g++ 13.3, 6 CPU / 6 GiB / 25 GiB |
| State | **Stopped** (`limactl list`) — start it before M0 |
| Guest dir | `~/httpfix` (new; do not reuse `~/review*`, `~/proj`, `~/srv`, `~/final`) |

### 2.2 Sync + build (run from the repo root on the host)

```bash
limactl start httpbuild                                    # if stopped
GUEST=httpbuild DEST=~/httpfix
tar -C "$PWD" --exclude=.git --exclude='build*' --exclude=pi-setup -cf - . | \
  timeout 120 limactl shell "$GUEST" -- tar -xf - -C ~/httpfix    # -C on the create side, always
limactl shell "$GUEST" -- bash -lc 'cd ~/httpfix && cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j"$(nproc)" 2>&1 | tail -20'
```

### 2.3 The gate (run at every milestone boundary, all four must pass)

```bash
# in the guest, from ~/httpfix
tests/run_tests.sh                                   # clean build 0 warnings, 95 parser checks, 37+ integration
cmake -B build-asan -DENABLE_ASAN=ON && cmake --build build-asan -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=1 python3 tests/test_http_server.py --binary ./build-asan/http_server
cmake -B build-tsan -DENABLE_TSAN=ON && cmake --build build-tsan -j"$(nproc)"
TSAN_OPTIONS=halt_on_error=1 python3 tests/test_http_server.py --binary ./build-tsan/http_server \
  || { echo "note: aarch64 ASLR needs the setarch -R wrapper (README:85)"; }
# and, the step that was missing before (F18):
grep -RInE 'Sanitizer|runtime error|ERROR:|SUMMARY:' /tmp/httpserver-test-*/server.log && echo "FAIL: sanitizer output found"
```

`halt_on_error=0` (README:80) becomes `1` once `Server.stop()` asserts exit status — otherwise a
TSan report is advisory. `tests/run_tests.sh` also needs to `exit 1` on `testsRun == 0` (F19).

### 2.4 Preserve the two already-verified patches before any VM cleanup

`REVIEW.md` §8 records that the verifiers' working trees are still in the VM:
`~/review-v28` = **F1** fix (37/37, no new warnings), `~/review-v33` = **F15** fix (95/95, 37/37,
idle footprint → 0). Extract them before deleting anything:

```bash
for d in v28 v33; do
  limactl shell httpbuild -- bash -lc "[ -d ~/review-$d ] && diff -u ~/review/$([ $d = v28 ] && echo src/connection.cpp || echo src/common.h) ~/review-$d/src/$([ $d = v28 ] && echo connection.cpp || echo common.h)"
done > /tmp/review-verified-patches.diff 2>&1
```

Reference only — re-derive against current HEAD, do not blind-apply.

---

## 3. Milestones

Order differs from `REVIEW.md` §3.6 in one respect: **the harness (F17–F20) moves to the front.**
The review proved the suite passes with EPOLLOUT re-arm removed (F17) and reports UBSan defects as
`OK` (F18); verifying six milestones of behavioural fixes with that instrument would produce green
results we already know are uninformative. Everything else follows §3.6.

Effort is relative, not hours: **S** ≤ 30 min incl. test, **M** ≈ 1 h, **L** needs a decision first.

### M0 — make the suite able to fail (no behaviour change except one test hook)

- [ ] **F18** `tests/test_http_server.py:148` (`Server.stop`) — assert `returncode == 0` on every
      stop, not just the 3 shutdown tests; grep the per-server log for
      `Sanitizer|runtime error|ERROR:|SUMMARY:` and fail with the log attached; `shutil.rmtree(self.tmp)`
      on success, keep it on failure. *(M)*
- [ ] **F18** `CMakeLists.txt:72` — add `-fno-sanitize-recover=undefined`; apply the strict warning
      set and the sanitizer flags to `parser_tests` too (`CMakeLists.txt:83-88`). *(S)*
- [ ] **F19** keyword filter — filter the already-loaded suite by `t.id()` instead of re-importing
      `__main__` (`AttributeError: module '__main__' has no attribute '__main__'`);
      `tests/run_tests.sh` exits non-zero when `testsRun == 0`. *(S)*
- [ ] **F20** `bench/bench.sh:49,51` — remove `exec` from the `wrk`/`ab` paths (it replaces the shell
      before the `EXIT` trap can kill the server → next run silently benchmarks a stale orphan);
      start the server with `--port-file` + poll instead of `sleep 0.5`; `cleanup` `wait`s for the PID. *(S)*
- [ ] **F17 hook** new `--sndbuf N` (default `0` = untouched) → `setsockopt(SO_SNDBUF)` on accepted
      sockets next to the existing `TCP_NODELAY`/`SO_KEEPALIVE` calls (`event_loop.cpp:333-336`) —
      *setting it explicitly disables autotuning*, which is the only
      way to get a real partial write on loopback; plus a `--verbose` line from `Connection::flush()`
      when it returns with `out_` non-empty (proof that an EAGAIN/partial write actually happened).
      Documented in `--help` and README as a test hook. *(M)*
- [ ] Save the verified F1/F15 patches (§2.4). *(S)*
- [ ] Housekeeping (§8): remove the 981 MB home-directory copy `~/proj` from the VM, and the 15
      `~/review-*` trees, **after** §2.4. Needs the OK asked for in **D7**. *(S)*

**Acceptance:** inject a leak, a UBSan defect and a UAF-at-exit into a scratch copy — all three make
the suite red (the review showed the UBSan one currently reports `OK` on all 37).
`tests/run_tests.sh --no-build -k shutdown` runs exactly the 3 shutdown tests; `-k nosuchtest` exits non-zero.

### M1 — lifetime and shutdown (the only UB, the only unbounded block)

- [ ] **F11** `src/event_loop.cpp:660-668`, `src/main.cpp:47` — on the `!pool_.stop_forced(250)`
      branch, reorder so `close_all(); log_stats();` run, then `std::fflush(nullptr); ::_exit(exit_code_);`.
      A detached worker still holds raw `loop`/`cfg` pointers (`src/connection.cpp:168-171`) and calls
      `loop->post_completion()` → ASan-confirmed heap-use-after-free today. `_exit` *is* the meaning of
      a hard shutdown; it also makes `wake_fd_.release()` moot. Fix the `~EventLoop` comment
      ("nothing can be posted anymore" is false) and README 212-217. *(M)*
- [ ] **F12** `src/thread_pool.{h,cpp}` + `src/event_loop.cpp:536` — add `std::atomic<std::size_t> running_`
      incremented/decremented around `job()` in `ThreadPool::run()`, expose `running()`, and require
      `pool_.running() == 0` in `shutdown_complete()`. Today a client RST during a handler reaps the
      connection, the loop declares itself drained, and the normal `pool_.stop()` blocks in an
      unbounded `pthread_join` with the signalfd no longer polled — budget expiry and second signal
      both ignored (observed exit at 5.21 s with `--shutdown-timeout 1`). *(M)*
- [ ] **F13** `src/connection.cpp:214` + `src/event_loop.cpp:518-523` — force `res.keep_alive = false`
      in `queue_response()` when `draining_`, so the final response advertises `Connection: close` and
      `finished()` becomes true as soon as it is flushed; call `reap()` immediately after
      `maybe_begin_drain()`'s `begin_draining()` loop so idle connections close in the same tick
      (README promises "idle connections close immediately"; with `--tick-ms 2000` the process took
      2.01 s). *(S)*

New tests: `test_20b` re-cut to `/slow/300` with `--shutdown-timeout 0.1` under ASan (the current
`/slow/9000` keeps the worker asleep past process exit, so the UAF is invisible);
`test_20c` RST-mid-`/slow/6000` + SIGTERM `--shutdown-timeout 1` + SIGINT×2 → assert `rc == 0` and
exit ≤ 2 s; `test_20d` in-flight `/slow/1500` + pipelined `/health` + SIGTERM → the `200` carries
`Connection: close` and `/health` is either answered or the close is advertised, never a bare RST.

### M2 — connection state machine (a dozen lines, mostly `connection.cpp`)

- [ ] **F1** `src/connection.cpp:54,117` — half-closed peer. In `on_worker_result()` after `flush()`:
      `if (peer_eof_) { drain_requests(); if (!in_flight_ && out_.empty()) close_after_ = true; }`;
      in `on_writable()` after `flush()`:
      `if (peer_eof_ && !in_flight_ && out_.empty()) close_after_ = true;`.
      Verified patch exists (§2.4). Today: second pipelined request after `shutdown(SHUT_WR)` is never
      answered, a spurious `408` follows, and with `--idle-timeout 0` the socket sits in CLOSE-WAIT
      until SIGTERM. *(M)*
- [ ] **F6** `src/connection.cpp:215,154` — `last_head_` is sticky: delete the check in
      `queue_response()` (the worker lambda already sets `res.send_body = false` for HEAD,
      `connection.cpp:184-185`), give `queue_error()` an explicit `bool head` parameter, and capture
      `req.is_head()` in `submit()` **before** `std::move(req)` for the `!queued` 503 branch.
      Today `HEAD /health` + idle → `408` with `Content-Length: 52` and zero body bytes. *(S)*
- [ ] **F9** `src/connection.cpp:308` — `if (parser_.in_progress() || !in_.empty())` → 408, else close
      silently. `HttpRequestParser::in_progress()` (`http_parser.h:53`) exists for exactly this and has
      no callers. Needs **D3**. *(S)*
- [ ] **F10** `src/connection.cpp:95`, `src/event_loop.cpp:387` — bounded lingering close
      (`shutdown(SHUT_WR)` + small discard budget) instead of `close()` with unread request bytes,
      which turns the `413`/`503` into an active RST (`TCPAbortOnClose` +1 per event). *(M)*
- [ ] **F17** real partial-write test — server `--sndbuf 8192`, client with `SO_RCVBUF=8192` reading
      slowly, `/big/3000000`: assert byte-exact body, keep-alive resumes afterwards, **and** that the
      server log recorded ≥1 partial write (so the test proves its own detector fired). Replaces the
      illusion at `tests/test_http_server.py:385,416,424`. *(M)*

New tests: `test_half_close_pipelined` (2 pipelined `GET`s then `shutdown(SHUT_WR)` → both answered,
clean EOF, no 408), the same after a 3 MB `/big` body, `test_head_then_408_has_no_body`,
`test_head_then_400_has_no_body`, `test_idle_silence_closes_without_408`,
`test_408_flushed_under_backpressure` (covers the ★ `tick()`/`set_interest()` gap in M4).

### M3 — parser and protocol hygiene

Each item = parser_tests check + one integration probe + README line, in one commit.

- [ ] **F3** `src/http_parser.cpp:553` — chunked **trailers are merged into `Request::headers`**
      (`parse_header_line(line, cur_, …)` pushes into the live request; `emit_request` hands the merged
      vector downstream). Add a `keep` flag to `parse_header_line()` (`http_parser.h:71`), validate but
      discard in `kChunkTrailer`, count trailer fields against `max_header_fields` separately, and fix
      the comment at `:551-552` that already claims they are ignored (README:181 says the same).
      Repro: `Content-Type: text/evil-from-trailer` supplied only as a trailer is reflected by `/echo`.
      Header-injection footgun for any future Host/Auth/X-Forwarded-\* logic. *(M)*
- [ ] **F4** `src/http_parser.cpp:208` — ignore unknown `Connection:` options instead of `kError`.
      Today `Connection: Upgrade, HTTP2-Settings` (what `curl --http2` and Java 11+ `HttpClient` send on
      plain `http://`) and `Connection: TE` (required by RFC 9110 §7.6.1 with `TE: trailers`) are hard
      `400` + close. Update `tests/parser_tests.cpp:164` (which enshrines the deviation) and the README
      bullet whose "protocol-confusion hazard" rationale does not hold — this server never emits `101`.
      Commit message flags it as a reversal of a documented deviation. *(S)*
- [ ] **F7** `src/http_parser.cpp:79` — in `kRequestLine`, `line_end == 0` → consume the CRLF and
      continue, capped at 1–2 blank lines per request (RFC 9112 §2.2 "SHOULD ignore at least one empty
      line"; legacy clients terminate POST bodies with CRLF). *(S)*
- [ ] **F8** `finish_headers()` — `>1` `Host` → 400; HTTP/1.1 with none → 400; internal whitespace →
      400 (RFC 9112 §3.2 MUST). **Ripple:** the harness `req()` (`tests/test_http_server.py:163`) sends
      **no Host header at all**, so this breaks nearly every existing test unless `req()` gains a default
      `Host: test` with an opt-out for the negative cases. See **D5**. *(M + ripple)*
- [ ] ★ parser batch (unverified in REVIEW §3.2 — verify each before changing):
      chunk framing bytes count toward `max_request_bytes` (`:347`, so a body within `max_body_bytes`
      can be rejected when chunks are small); chunk-size line length bounded only while incomplete
      (`:496`); missing CRLF after chunk data detected late (`:452`); request-line limit off-by-one when
      CR/LF split across reads (`:361`); `HTTP/1.2` → treat as 1.1 and `HTTP/2.0` → `505` instead of
      `400` (`:118`); `Content-Length > 2^52` → 413 not 400 (`http_common.cpp:126`); unknown `Expect`
      → 417 not 400 (`:293`); dead limit check (`http_parser.cpp:150`). *(S each)*

### M4 — resources and denial of service

- [ ] **F15** `src/common.h:42-48,72-75` — `ByteBuf::release_if_large()`: swap `data_` with an empty
      `std::string` when `capacity() > 64 KiB`, called from `consume()` when the buffer empties and
      from `clear()`. Verified patch exists (§2.4); measured RSS 3.5 MB → 363.6 MB for 32 idle
      keep-alives after one `/big/8388608` each, i.e. 8 MiB pinned per connection, worst case
      4096 × 8 MiB. The parser already releases its body buffer above 64 KiB
      (`http_parser.cpp:51-53`); `ByteBuf` is the inconsistency. *(S)*
- [ ] **F2** `src/connection.cpp:94-98,54` — turn the aggregate input cap into **backpressure**:
      add `&& in_.size() < cfg_.max_request_bytes` to `want_read()` (the existing rescan machinery
      resumes reading as `drain_requests()` shrinks `in_`), delete the 413 block, and keep 413 only for
      a *single* request that cannot complete inside the cap (`http_parser.cpp:347` `kNeedMore`).
      Today 300 k pipelined 42-byte `GET`s get one `413` and zero `200`s, and README's "throttled by
      their own TCP window" describes a design the code does not implement. Needs **D1**. *(M)*
- [ ] **F5** `src/connection.cpp:93,248,294` — add a **request/header deadline** independent of byte
      activity (`last_activity_` resets on every `recv()`/`send()`, so 1 byte / 0.7 s held a connection
      35.9 s with `--idle-timeout 1`; ~1 KB/s from one host sustains all 4096 slots, no per-IP cap).
      Start the deadline at the first byte of a request, keep the idle timer for between-request
      silence, optional per-IP connection cap. Needs **D2**. *(L)*
- [ ] **F14** `src/event_loop.cpp:318-325,490-492` — `getrlimit`/`setrlimit` at startup, clamp
      `max_connections` to `rlim_cur − headroom` and log it (default 4096 exceeds a stock 1024 ulimit);
      keep a spare `open("/dev/null")` fd for the close-accept-503-close EMFILE trick so clients stop
      hanging in the backlog with no `503` (measured: `ulimit -n 40`, 60 clients → 33 served,
      27 stalled 15.3 s, 0 × 503); fix `tick()` clearing `accept_retry_` before `accept_batch()`, which
      makes the "log once" guard dead code. Needs **D6**. *(M)*
- [ ] **F16** `src/handler.cpp:173-178,198-203` — `/slow/<ms>` is an unauthenticated worker-pool
      starvation primitive (`--workers 2 --queue-size 2`, 12 × `/slow/4000` → 8 × 503 and 3 concurrent
      `/health` → 3 × 503; ~1032 connections at defaults). Gate behind `--allow-slow`, default off,
      and/or a reserved fast lane. **Ripple:** 4 tests use `/slow`
      (`test_http_server.py:482,530,575,601`) → they must pass the flag. Needs **D4**. *(M)*
- [ ] ★ loop/config batch: `tick()` never calls `set_interest()`, so a `408` that hits EAGAIN is never
      flushed (`event_loop.cpp:495`); accept-batch remainder deferred to the next *tick* (up to 200 ms)
      rather than the next loop iteration (`:361`); accept errors other than EAGAIN/EMFILE/ECONNABORTED
      abandon the edge (`:326`); unbounded `fprintf(stderr)` on the loop thread (`:51`);
      `parse_size()` accepts negatives — `--max-body -1` → 2^64−1 (`config.cpp:25`); `parse_double()`
      accepts `nan`/`inf` → UB in the chrono cast (`config.cpp:37`); `ThreadPool` comment says it joins
      when `pthread_create` fails (`thread_pool.cpp:23`); a job that exits without posting leaves
      `in_flight_` set forever (`connection.cpp:188`). *(S each)*

### M5 — documentation, written from tool output

- [ ] README `:8-9` — regenerate: **2,545** `.cpp` lines across **8** `.cpp` + 8 headers, **2** mutexes,
      **2** `thread_local` statics (claim today: "~1900 lines, seven source files, three mutexes, no
      thread-local state" — all four wrong).
- [ ] README `:181` trailers, `:68` (`-k` example, only true after F19), the "throttled by their own
      TCP window" claim (only true after F2), `:212-217` forced-shutdown argument (after F11),
      the sanitizer wording (only after F18 makes it assertable).
- [ ] Add the F4 / F7 / F8 / F9 / F16 behaviours and the `--max-request` auto-raise, `--idle-timeout 0`,
      and the 256-field header cap to "Deliberate deviations" (`README.md:198`).
- [ ] Tighten the over-loose assertions REVIEW §3.2 flags: `test_20` accepts a hung/reset connection as
      success (`:590`), `test_09c` accepts 400 for oversized headers, hiding the 413 contract (`:306`),
      and add a 400-path assertion that the connection actually closes.
- [ ] Fill the named coverage gaps: EPOLLERR/EINTR/SIGPIPE, `--bind 0.0.0.0` / `::1`,
      shutdown-budget expiry, `--max-output` backpressure, worker exception → 500, pipelining mixed with
      `Connection: close`, `HEAD` followed by an error.

---

## 4. Test inventory (what gets added)

| Test | Closes | Server flags |
|---|---|---|
| `test_half_close_pipelined` (+ 3 MB body variant) | F1 | `--idle-timeout 2`, `--idle-timeout 0` |
| `test_head_then_408_has_no_body`, `test_head_then_400_has_no_body` | F6 | default |
| `test_idle_silence_closes_without_408` | F9 | short `--idle-timeout` |
| `test_partial_write_real` (locked `SO_SNDBUF`, slow reader, byte-exact + log assertion) | F17 | `--sndbuf 8192 --verbose` |
| `test_pipelining_under_aggregate_cap` (300 k small GETs, 30 × 4 KB POSTs) | F2 | `--max-request 69632` |
| `test_trailer_not_a_header` (`/echo`, `/info`), `test_connection_upgrade_accepted`, `test_stray_crlf_before_request`, `test_host_enforcement` | F3, F4, F7, F8 | default |
| `test_header_deadline` (1 byte / 0.7 s) | F5 | `--header-timeout` — **D2** |
| `test_fd_exhaustion_returns_503` | F14 | run under low `ulimit -n` |
| `test_slow_disabled_by_default`, `test_slow_enabled_with_flag` | F16 | `--allow-slow` |
| `test_keepalive_memory_released` (32 × `/big/8388608`, then RSS check via `/proc/<pid>/status`) | F15 | default |
| `test_20b` re-cut, `test_20c` orphaned-job budget, `test_20d` drain advertises `Connection: close` | F11, F12, F13 | short `--shutdown-timeout` |
| `test_lingering_close_no_rst` | F10 | default |
| parser_tests: trailer isolation, unknown Connection option, leading CRLF, Host matrix, `HTTP/1.2`/`2.0`, `Expect`, `CL > 2^52` | M3 | n/a |

## 5. Mutation checklist (run once per finding, not committed)

For each fix, break the mechanism and confirm the new test goes red, then restore:

| Break | Must go red |
|---|---|
| remove the EPOLLOUT re-arm (i.e. `return` early in `flush()`) | `test_partial_write_real` |
| remove the `peer_eof_` handling added for F1 | `test_half_close_pipelined` |
| remove `pool_.running()` from `shutdown_complete()` | `test_20c` |
| remove the `_exit` on the detach branch, build ASan | `test_20b` (UAF report in the log → F18 must surface it) |
| make `release_if_large()` a no-op | `test_keepalive_memory_released` |
| re-add the 413 block in `on_readable()` | `test_pipelining_under_aggregate_cap` |
| inject a `new` without `delete` in the handler | the suite (F18 end-to-end proof) |
| inject `int z = *(int*)nullptr;`-class UB (e.g. `-(unsigned)1/0`) | the suite (needs `-fno-sanitize-recover=undefined`) |

## 6. Suggested slices

If you want a smaller first review point, take **M0 + M1 + F1 + F6 + F13 + F15**:
that is the harness, the only UB, the only unbounded block, and the two state-machine/resource bugs
with verified patches — most of the robustness/correctness score back for well under half the work.
Then M2 (rest), M3, M4, M5 as separate review points.

## 7. Decisions needed before starting (`D1`–`D8`)

**D1 · F2** — aggregate input cap becomes *backpressure*, `413` reserved for a single oversized request?
   (Changes the observable contract and the README text.)
**D2 · F5** — new flag name/default (`--header-timeout 15`? reuse `idle_timeout`?) and does
   `--idle-timeout 0` keep meaning "all timers off"? It currently disables the idle timer entirely,
   itself an undocumented deviation.
**D3 · F9** — close *silently* on a connection that never sent a byte, instead of the requirement list's
   "timeout → 408"? (nginx/Apache behaviour; needs a README deviation line.)
**D4 · F16** — `/slow` default **off** behind `--allow-slow` (breaks the demo scripts unless they pass it),
   or default-on + per-IP concurrency cap?
**D5 · F8** — strict `Host` now (implies editing the harness `req()` to send `Host:` by default, touching
   all 37 tests) or bundle it with M5?
**D6 · F14** — OK with `max_connections` being silently clamped at startup (loudly logged) rather than a
   hard startup error when `--max-connections` exceeds the rlimit?
**D7 · VM housekeeping** — permission to `rm -rf ~/proj` (the leftover 981 MB home-directory copy with
   shell histories, `.claude.json`, `.pi/agent`, `.hermes`) and the 15 `~/review-*` trees, after §2.4.
**D8 · Landing policy** — branch + you review each milestone, or straight commits to `main`?

## 8. Explicitly out of scope (say the word to pull any in)

- The 24 non-★ §3.2 nits (prefix routing `/big123`, 204 `Content-Type`, `X-Content-Type-Options`,
  obs-text reflection, `--max-output` being inert at defaults, IPv6-disabled bind, 2× `tick_ms` slip, …)
  — one dedicated pass after M4, since none is confirmed.
- §4–§6 of REVIEW.md (session/harness configuration): already addressed by `pi-setup/`.
- Structural changes (multiple worker threads touching connections, io_uring, HTTP/2).

## 9. Definition of done

- F1–F20 closed, each with a commit named after the finding and a test that fails without it (§5).
- §2.3 gate green on the final tree: clean build 0 warnings, parser tests, integration tests,
  ASan+UBSan+LSan, TSan, and **no sanitizer output in any per-server log**.
- README re-measured from tool output; every new deviation listed; the sanitizer claim re-worded to
  exactly what the suite now asserts.
- `REVIEW.md` §3.5 table annotated with which claims are now true, and §8 housekeeping resolved.

## 10. Progress tracker

| Milestone | Findings | Status |
|---|---|---|
| M0 harness | F17(hook), F18, F19, F20 | ☐ blocked on **D7** |
| M1 lifetime/shutdown | F11, F12, F13 | ☐ |
| M2 state machine | F1, F6, F9, F10, F17 | ☐ blocked on **D3** |
| M3 parser | F3, F4, F7, F8 + ★ | ☐ blocked on **D5** |
| M4 resources | F2, F5, F14, F15, F16 + ★ | ☐ blocked on **D1, D2, D4, D6** |
| M5 docs | §3.5, §3.2 doc items | ☐ |
