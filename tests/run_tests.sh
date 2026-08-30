#!/usr/bin/env bash
# Build (unless --no-build) and run the parser unit tests + the integration
# suite against a real server process.
#
#   tests/run_tests.sh                       # build ./build, run everything
#   tests/run_tests.sh --binary build/http_server --no-build
#   tests/run_tests.sh -k shutdown           # only tests matching "shutdown"
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
BUILD_DIR="$ROOT/build"
BINARY=""
BUILD=1
PY=python3
FILTER=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary)   BINARY="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    -j)         JOBS="$2"; shift 2 ;;
    -k)         FILTER=(-k "$2"); shift 2 ;;
    -h|--help)  sed -n '2,8p' "$0"; exit 0 ;;
    *)          echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "${JOBS:-}" ]] || JOBS="$(nproc 2>/dev/null || echo 4)"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "error: this server is Linux-only (epoll/accept4/eventfd/signalfd); running on $(uname -s)" >&2
  exit 1
fi

if [[ "$BUILD" == "1" ]]; then
  echo "== configuring ($BUILD_DIR)"
  cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
  echo "== building"
  cmake --build "$BUILD_DIR" -j "$JOBS"
fi

if [[ -z "$BINARY" ]]; then BINARY="$BUILD_DIR/http_server"; fi
if [[ ! -x "$BINARY" ]]; then echo "error: binary not found or not executable: $BINARY" >&2; exit 1; fi
BINARY="$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")"

echo "== parser unit tests"
if [[ -x "$BUILD_DIR/parser_tests" ]]; then
  "$BUILD_DIR/parser_tests"
else
  echo "   (parser_tests not built, skipping)"
fi

echo "== integration tests ($BINARY)"
"$PY" "$ROOT/tests/test_http_server.py" --binary "$BINARY" "${FILTER[@]+"${FILTER[@]}"}"
echo "== all tests passed"
