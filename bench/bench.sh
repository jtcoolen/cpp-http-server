#!/usr/bin/env bash
# Benchmark helper.  Uses wrk, then ab, then a built-in Python client so that
# there is always *some* way to load the server.
#
#   bench/bench.sh                       # starts its own server on :8080
#   bench/bench.sh --port 9000 --conns 100 --requests 50000 --duration 10
#   bench/bench.sh --url http://127.0.0.1:8080/big/65536 --external
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
BINARY="$ROOT/build/http_server"
HOST=127.0.0.1
PORT=8080
CONNS=50
REQUESTS=100000
DURATION=10
URL_PATH=/
EXTERNAL=0
BIN_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) BINARY="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --conns|-c) CONNS="$2"; shift 2 ;;
    --requests|-n) REQUESTS="$2"; shift 2 ;;
    --duration|-d) DURATION="$2"; shift 2 ;;
    --path) URL_PATH="$2"; shift 2 ;;
    --external) EXTERNAL=1; shift ;;
    *) BIN_ARGS+=("$1"); shift ;;
  esac
done

URL="http://$HOST:$PORT$URL_PATH"
SRV_PID=""
cleanup() { [[ -n "$SRV_PID" ]] && kill -TERM "$SRV_PID" 2>/dev/null || true; }
trap cleanup EXIT

if [[ "$EXTERNAL" == "0" ]]; then
  echo "== starting $BINARY on $HOST:$PORT"
  "$BINARY" --bind 0.0.0.0 --port "$PORT" "${BIN_ARGS[@]}" &
  SRV_PID=$!
  sleep 0.5
fi

echo "== target $URL  conns=$CONNS requests=$REQUESTS duration=${DURATION}s"
if command -v wrk >/dev/null 2>&1; then
  exec wrk -t"$(nproc)" -c"$CONNS" -d"${DURATION}s" --latency "$URL"
elif command -v ab >/dev/null 2>&1; then
  exec ab -n "$REQUESTS" -c "$CONNS" -k "$URL"
fi

echo "== neither wrk nor ab found; using the built-in Python client"
python3 - "$HOST" "$PORT" "$CONNS" "$DURATION" "$URL_PATH" <<'PY'
import socket, sys, threading, time
host, port, conns, dur, path = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
req = ("GET %s HTTP/1.1\r\nHost: bench\r\nConnection: keep-alive\r\n\r\n" % path).encode()
stop = time.time() + dur
count = 0
bytes_ = 0
lock = threading.Lock()
errors = 0
def worker():
    global count, bytes_, errors
    try:
        s = socket.create_connection((host, port), timeout=10)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        buf = b""
        while time.time() < stop:
            s.sendall(req)
            # read status + headers, then Content-Length bytes (keep-alive safe)
            while b"\r\n\r\n" not in buf:
                buf += s.recv(65536)
            head, buf = buf.split(b"\r\n\r\n", 1)
            status = int(head.split(b" ")[1])
            if status != 200:
                with lock: errors += 1
                continue
            cl = 0
            for line in head.split(b"\r\n")[1:]:
                if line.lower().startswith(b"content-length:"):
                    cl = int(line.split(b":")[1])
            while len(buf) < cl:
                buf += s.recv(65536)
            body, buf = buf[:cl], buf[cl:]
            with lock:
                count += 1
                bytes_ += len(body)
        s.close()
    except Exception:
        with lock: errors += 1
ts = [threading.Thread(target=worker) for _ in range(conns)]
t0 = time.time()
for t in ts: t.start()
for t in ts: t.join()
el = time.time() - t0
print("requests: %d  errors: %d  time: %.2fs  req/s: %.0f  MB/s: %.1f"
      % (count, errors, el, count / el, bytes_ / el / 1e6))
PY
