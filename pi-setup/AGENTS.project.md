# Project instructions — cpp-http-server

Copy this to `AGENTS.md` in the repo root. It is loaded automatically by pi (and
Claude Code) at startup for this project.

## What this is
A Linux-only HTTP/1.1 server: one epoll(EPOLLET) event loop + a worker thread pool.
See `README.md` for architecture and `REVIEW.md` for the independent review (20
verified findings; start from §3.6 for the fix order).

## Building and testing (this is a macOS host; the code needs Linux)
- Do **not** try to build on macOS — there is no epoll/accept4/eventfd/signalfd.
- Use the lima VM `httpbuild`:
  ```sh
  limactl start httpbuild
  git -C /Users/julian/http-server archive HEAD | \
    limactl shell httpbuild -- bash -c 'rm -rf ~/work && mkdir ~/work && tar -xf - -C ~/work'
  limactl shell httpbuild -- bash -lc 'cd ~/work && cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j6'
  limactl shell httpbuild -- bash -lc 'cd ~/work && ./build/parser_tests && python3 tests/test_http_server.py --binary ./build/http_server'
  ```
- Sanitizers (the review used these): `-DENABLE_ASAN=ON` and `-DENABLE_TSAN=ON`
  (TSan needs `setarch aarch64 -R` on this arm64 guest — see README).

## Rules specific to this repo
- Always sync into the VM with `tar -C <abs> …` or `git archive` — never a bare
  `tar -cf - .`. A missing `-C`/`cd` in the original build copied `$HOME` into the
  VM and filled the disk.
- After changing the server, verify the negative: for a bug fix, add a raw-socket
  test that fails on the old binary first. The existing "partial write" tests do
  **not** exercise the EPOLLOUT path (`REVIEW.md` F17) — do not trust them as-is.
- When you claim a sanitizer run is clean, grep every server log and assert exit
  code 0; the harness currently checks only 3 of 37 (`REVIEW.md` F18).
- Do not delete files in the VM without asking; `~/proj`, `~/srv`, `~/final`,
  `~/review*` there are leftovers from earlier sessions (see `REVIEW.md` §6).
