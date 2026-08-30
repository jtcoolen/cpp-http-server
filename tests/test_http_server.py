#!/usr/bin/env python3
"""Integration tests for the epoll/EPOLLET HTTP/1.1 server.

Real server process, real sockets, raw bytes wherever the wire format matters.
  usage: test_http_server.py --binary ./build/http_server [-v]
"""
import argparse, os, signal, socket, struct, subprocess, sys, tempfile, threading, time, unittest

HOST4, HOST6, CRLF = "127.0.0.1", "::1", b"\r\n"
BINARY = "./build/http_server"


class HttpResponse:
    def __init__(self, version, status, reason, headers, body):
        self.version, self.status, self.reason, self.headers, self.body = version, status, reason, headers, body
    def header(self, name):
        for k, v in self.headers:
            if k.lower() == name.lower():
                return v
        return None
    def int_header(self, name):
        v = self.header(name)
        return None if v is None else int(v)
    def __repr__(self):
        return "<%d %s hdrs=%s body=%d>" % (self.status, self.reason, self.headers, len(self.body))


class Conn:
    """Raw client with just enough response parsing for the assertions."""
    def __init__(self, host, port, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
    def send(self, data):
        self.sock.sendall(data)
    def _line(self):
        while CRLF not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("closed while reading line; have %r" % self.buf[:120])
            self.buf += chunk
        line, self.buf = self.buf.split(CRLF, 1)
        return line.decode("latin-1")
    def _exactly(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("closed early, needed %d" % n)
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out
    def read_response(self, want_head=False, slow_read=False):
        version, status, reason = self._line().split(" ", 2)
        headers = []
        while True:
            line = self._line()
            if line == "":
                break
            name, _, value = line.partition(":")
            headers.append((name.strip(), value.strip()))
        body = b""
        if not want_head and int(status) not in (204, 304):
            cl = te = None
            for k, v in headers:
                lk = k.lower()
                if lk == "content-length":
                    cl = int(v)
                elif lk == "transfer-encoding":
                    te = v.lower()
            if cl is not None:
                if slow_read:
                    while len(body) < cl:
                        body += self._exactly(min(4096, cl - len(body)))
                else:
                    body = self._exactly(cl)
            elif te and "chunked" in te:
                while True:
                    size = int(self._line().split(";")[0], 16)
                    if size == 0:
                        self._line()
                        break
                    body += self._exactly(size)
                    self._line()
            else:
                self.sock.settimeout(5.0)
                try:
                    while True:
                        chunk = self.sock.recv(65536)
                        if not chunk:
                            break
                        body += chunk
                except socket.timeout:
                    pass
        return HttpResponse(version, int(status), reason, headers, body)
    def expect_closed(self, timeout=4.0):
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return False, "still open (timed out)"
        except (ConnectionResetError, OSError):
            return True, "reset"
        if data == b"":
            return True, "eof"
        return False, "unexpected bytes %r" % data[:60]
    def rst(self):
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        except OSError:
            pass
    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Server:
    def __init__(self, args=(), host=HOST4):
        self.args, self.host = list(args), host
        self.tmp = tempfile.mkdtemp(prefix="httpserver-test-")
        self.port_file = os.path.join(self.tmp, "port")
        self.log_path = os.path.join(self.tmp, "server.log")
        self.proc, self.port = None, None
    def __enter__(self):
        self.start(); return self
    def __exit__(self, *exc):
        self.stop(); return False
    def start(self):
        cmd = [BINARY, "--port", "0", "--port-file", self.port_file] + self.args
        self.logf = open(self.log_path, "wb")
        self.proc = subprocess.Popen(cmd, stdout=self.logf, stderr=subprocess.STDOUT)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                text = open(self.port_file).read().strip()
            except OSError:
                text = ""
            if text.isdigit():
                self.port = int(text); return
            if self.proc.poll() is not None:
                raise RuntimeError("server died at startup: %s" % self.log())
            time.sleep(0.02)
        raise RuntimeError("no port reported; " + self.log())
    def log(self):
        try:
            return open(self.log_path, "rb").read().decode("utf-8", "replace")
        except OSError:
            return ""
    def stop(self, sig=signal.SIGTERM, timeout=20):
        if self.proc is None:
            return None
        if self.proc.poll() is not None:
            return self.proc.returncode
        self.proc.send_signal(sig)
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait(timeout=5); return self.proc.returncode
    def conn(self, host=None, timeout=10.0):
        return Conn(host or self.host, self.port, timeout=timeout)


def req(method, target, version="1.1", headers=(), body=None, cl=True):
    out = ("%s %s HTTP/%s\r\n" % (method, target, version)).encode("latin-1")
    for k, v in headers:
        out += ("%s: %s\r\n" % (k, v)).encode("latin-1")
    if body is not None and cl and not any(h[0].lower() == "content-length" for h in headers):
        out += ("Content-Length: %d\r\n" % len(body)).encode("latin-1")
    out += CRLF
    return out + (body or b"")


class TestBasics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server(["--verbose"]); cls.server.start()
    @classmethod
    def tearDownClass(cls):
        sys.stderr.write(cls.server.log()[-3000:] + "\n"); cls.server.stop()

    # 1
    def test_01_get(self):
        c = self.server.conn(); c.send(req("GET", "/")); r = c.read_response()
        self.assertEqual(r.status, 200); self.assertEqual(r.version, "HTTP/1.1")
        self.assertEqual(r.int_header("Content-Length"), len(r.body))
        self.assertIn(b"cpp-http", r.body); c.close()
    def test_01b_ipv6(self):
        c = self.server.conn(host=HOST6); c.send(req("GET", "/health")); r = c.read_response()
        self.assertEqual(r.status, 200); self.assertEqual(r.body, b"ok\n"); c.close()

    # 2
    def test_02_head_length_no_body(self):
        c = self.server.conn()
        c.send(req("HEAD", "/")); r = c.read_response(want_head=True)
        self.assertEqual(r.status, 200); self.assertEqual(r.body, b"")
        length = r.int_header("Content-Length"); self.assertEqual(length, 47)
        c.send(req("GET", "/")); g = c.read_response()
        self.assertEqual(g.int_header("Content-Length"), length)
        self.assertEqual(len(g.body), length)
        c.send(req("HEAD", "/missing")); h4 = c.read_response(want_head=True)
        self.assertEqual(h4.status, 404); self.assertEqual(h4.body, b"")
        self.assertGreater(h4.int_header("Content-Length"), 0); c.close()

    # 3
    def test_03_post_content_length(self):
        payload = b"line one\r\ntwo\n\r\n" * 17
        c = self.server.conn()
        c.send(req("POST", "/echo", headers=[("Content-Type", "text/plain")], body=payload))
        r = c.read_response()
        self.assertEqual(r.status, 200); self.assertEqual(r.body, payload)
        self.assertEqual(r.header("Content-Type"), "text/plain")
        self.assertEqual(r.int_header("Content-Length"), len(payload))
        c.send(req("POST", "/info", body=b"z" * 4242))
        self.assertIn(b'"body_bytes":4242', c.read_response().body); c.close()

    # 4
    def test_04_options_star(self):
        c = self.server.conn(); c.send(req("OPTIONS", "*")); r = c.read_response()
        self.assertEqual(r.status, 204); self.assertEqual(r.body, b"")
        self.assertIsNone(r.header("Content-Length"), "204 must not carry Content-Length")
        allow = r.header("Allow") or ""
        for m in ("GET", "HEAD", "POST", "OPTIONS"):
            self.assertIn(m, allow)
        c.send(req("OPTIONS", "/echo")); self.assertEqual(c.read_response().status, 204)
        c.close()

    # 5
    def test_05_not_found(self):
        c = self.server.conn(); c.send(req("GET", "/nope")); r = c.read_response()
        self.assertEqual(r.status, 404)
        self.assertEqual(r.int_header("Content-Length"), len(r.body)); c.close()

    # 6
    def test_06_method_not_allowed(self):
        c = self.server.conn()
        for m in ("PUT", "DELETE", "PATCH", "TRACE", "FOOBAR"):
            c.send(req(m, "/")); r = c.read_response()
            self.assertEqual(r.status, 405, "%s -> %r" % (m, r))
            self.assertIn("GET", r.header("Allow") or "")
            self.assertIn("OPTIONS", r.header("Allow") or "")
        c.send(req("GET", "/boom")); self.assertEqual(c.read_response().status, 500)
        c.close()

    # 7
    def test_07_malformed_request_line(self):
        cases400 = [b"GET\r\n\r\n", b"GET / HTTP/1.0.1\r\n\r\n", b"GET / HTTP/2.0\r\n\r\n",
                    b"GET http://example.com/ HTTP/1.1\r\n\r\n", b"GET //example.com/p HTTP/1.1\r\n\r\n",
                    b"GET example.com:80 HTTP/1.1\r\n\r\n", b"GET /a#b HTTP/1.1\r\n\r\n",
                    b"GET  / HTTP/1.1\r\n\r\n", b"\r\n\r\n", b"GET /\tHTTP/1.1\r\n\r\n",
                   ]
        for raw in cases400:
            c = self.server.conn(); c.send(raw)
            try:
                r = c.read_response()
            except EOFError:
                self.fail("400 expected for %r (got close)" % raw)
            self.assertEqual(r.status, 400, "%r -> %r" % (raw, r))
            c.close()
        # A syntactically valid method token we do not implement is 405, not 400.
        # Methods are case-sensitive per RFC 7230, so "get" is an extension method.
        c = self.server.conn()
        for raw in (b"GCT / HTTP/1.1\r\n\r\n", b"get / HTTP/1.1\r\n\r\n"):
            c.send(raw)
            self.assertEqual(c.read_response().status, 405, "%r" % raw)
        c.close()

    # 8
    def test_08_malformed_headers(self):
        cases = [b"GET / HTTP/1.1\r\nBad Header: x\r\n\r\n", b"GET / HTTP/1.1\r\n:nov\r\n\r\n",
                 b"GET / HTTP/1.1\r\nNoColon\r\n\r\n", b"GET / HTTP/1.1\r\nX: a\r\n folded\r\n\r\n",
                 b"GET / HTTP/1.1\r\nX\x01: v\r\n\r\n", b"GET / HTTP/1.1\r\nX: a\x01v\r\n\r\n",
                 b"GET / HTTP/1.1\nHost: x\n\n", b"GET /\r\nEvil: yes HTTP/1.1\r\n\r\n"]
        for raw in cases:
            c = self.server.conn(); c.send(raw)
            try:
                r = c.read_response()
            except EOFError:
                self.fail("400 expected for %r" % raw)
            self.assertEqual(r.status, 400, "%r -> %r" % (raw, r))
            self.assertIsNone(r.header("Evil")); c.close()
        c = self.server.conn(); c.send(b"GET / HTTP/1.1\r\nX: a\r\nY: b\r\n\r\n")
        self.assertEqual(c.read_response().status, 200); c.close()

    # 9
    def test_09_oversized_content_length(self):
        c = self.server.conn()
        c.send(b"POST /echo HTTP/1.1\r\nContent-Length: 999999999999\r\n\r\n")
        r = c.read_response(); self.assertEqual(r.status, 413)
        ok, why = c.expect_closed(); self.assertTrue(ok, "413 must close: " + why); c.close()
    def test_09b_body_over_limit(self):
        with Server(["--max-body", "4096"]) as s:
            c = s.conn()
            c.send(req("POST", "/echo", body=b"a" * 5000))
            self.assertEqual(c.read_response().status, 413)
            ok, why = c.expect_closed(); self.assertTrue(ok, "413 closes: " + why)
            c.close()
            c2 = s.conn()  # 413 closes the connection, so use a fresh one
            c2.send(req("POST", "/echo", body=b"a" * 400))
            r = c2.read_response()
            self.assertEqual(r.status, 200); self.assertEqual(r.body, b"a" * 400)
            c2.close()
    def test_09c_oversized_headers_and_line(self):
        c = self.server.conn()
        c.send(b"GET / HTTP/1.1\r\nX-Big: " + b"a" * (128 * 1024) + b"\r\n\r\n")
        self.assertIn(c.read_response().status, (400, 413, 431)); c.close()
        c = self.server.conn(); c.send(b"GET /" + b"a" * (128 * 1024) + b" HTTP/1.1\r\n\r\n")
        self.assertIn(c.read_response().status, (400, 413, 414)); c.close()

    # 10
    def test_10_duplicate_content_length(self):
        for raw in (b"POST /echo HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\nabcd",
                    b"POST /echo HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\nabcd",
                    b"POST /echo HTTP/1.1\r\nContent-Length: 4, 4\r\n\r\nabcd",
                    b"POST /echo HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
                    b"POST /echo HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
                    b"POST /echo HTTP/1.1\r\nContent-Length: 007\r\n\r\nabc"):
            c = self.server.conn(); c.send(raw)
            try:
                r = c.read_response()
            except EOFError:
                self.fail("400 expected for %r" % raw)
            self.assertEqual(r.status, 400, "%r -> %r" % (raw, r)); c.close()

    # 11
    def test_11_content_length_with_chunked(self):
        for raw in (b"POST /echo HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
                    b"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                    b"POST /echo HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
                    b"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n0\r\n\r\n"):
            c = self.server.conn(); c.send(raw)
            try:
                r = c.read_response()
            except EOFError:
                self.fail("400 expected for %r" % raw)
            self.assertEqual(r.status, 400, "%r -> %r" % (raw, r)); c.close()

    # 12
    def test_12_chunked_post(self):
        c = self.server.conn()
        c.send(b"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Type: x/a\r\n\r\n"
               b"5\r\nhello\r\n8;ext=1\r\nw\xd0\x9c\xd0\xb8!!!\r\n0\r\nX-Trail: 1\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.status, 200, "%r" % r)
        self.assertEqual(r.body, b"hellow\xd0\x9c\xd0\xb8!!!")
        self.assertEqual(r.header("Content-Type"), "x/a")
        c.send(b"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
               b"5\r\nhelloJUNK\r\n0\r\n\r\n")
        self.assertEqual(c.read_response().status, 400)
        c.close()
    def test_12b_chunked_dribbled(self):
        c = self.server.conn()
        wire = (b"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                b"10\r\n0123456789abcdef\r\n3\r\nxyz\r\n0\r\n\r\n")
        for i in range(0, len(wire), 5):
            c.send(wire[i:i + 5]); time.sleep(0.003)
        r = c.read_response()
        self.assertEqual(r.status, 200); self.assertEqual(r.body, b"0123456789abcdefxyz"); c.close()

    # 13
    def test_13_http_1_0(self):
        c = self.server.conn(); c.send(req("GET", "/health", version="1.0"))
        r = c.read_response()
        self.assertEqual(r.version, "HTTP/1.0"); self.assertEqual(r.status, 200)
        self.assertEqual(r.header("Connection"), "close")
        ok, why = c.expect_closed(); self.assertTrue(ok, "1.0 must close: " + why); c.close()
    def test_13b_http_1_0_keepalive(self):
        c = self.server.conn()
        c.send(req("GET", "/health", version="1.0", headers=[("Connection", "keep-alive")]))
        r = c.read_response(); self.assertEqual(r.status, 200)
        self.assertEqual(r.header("Connection"), "keep-alive")
        c.send(req("GET", "/info", version="1.0", headers=[("Connection", "keep-alive")]))
        r2 = c.read_response(); self.assertEqual(r2.status, 200)
        self.assertIn(b'"version":"1.0"', r2.body); c.close()

    # 15
    def test_15_http_1_1_keepalive(self):
        c = self.server.conn()
        for _ in range(6):
            c.send(req("GET", "/health")); r = c.read_response()
            self.assertEqual(r.status, 200); self.assertEqual(r.body, b"ok\n")
        c.send(req("GET", "/health", headers=[("Connection", "close")]))
        r = c.read_response(); self.assertEqual(r.status, 200)
        self.assertEqual(r.header("Connection"), "close")
        ok, why = c.expect_closed(); self.assertTrue(ok, "Connection: close honoured: " + why); c.close()
    def test_15b_close_after_large_body(self):
        c = self.server.conn(); payload = b"y" * 300000
        c.send(req("POST", "/echo", body=payload, headers=[("Connection", "close")]))
        r = c.read_response(); self.assertEqual(r.body, payload)
        ok, why = c.expect_closed(); self.assertTrue(ok, why); c.close()

    # 16
    def test_16_pipelining(self):
        c = self.server.conn()
        c.send(req("GET", "/health") + req("GET", "/info") + req("HEAD", "/") +
               req("POST", "/echo", body=b"pipe") + req("GET", "/missing") + req("OPTIONS", "*"))
        want = [200, 200, 200, 200, 404, 204]
        got, bodies = [], []
        for i in range(6):
            r = c.read_response(want_head=(i == 2))
            got.append(r.status); bodies.append(r.body)
        self.assertEqual(got, want)
        self.assertEqual(bodies[0], b"ok\n"); self.assertIn(b'"method":"GET"', bodies[1])
        self.assertEqual(bodies[2], b""); self.assertEqual(bodies[3], b"pipe")
        c.send(req("GET", "/health")); self.assertEqual(c.read_response().status, 200)
        c.close()
    def test_16b_pipelining_50(self):
        c = self.server.conn(); c.send(req("GET", "/info") * 50)
        for i in range(50):
            r = c.read_response()
            self.assertEqual(r.status, 200, "pipelined response %d" % i)
            self.assertIn(b'"body_bytes":0', r.body)
        c.close()

    # 17
    def test_17_partial_writes(self):
        c = self.server.conn(); c.send(req("GET", "/big/2000000"))
        r = c.read_response(slow_read=True)
        self.assertEqual(r.status, 200); self.assertEqual(len(r.body), 2000000)
        self.assertEqual(r.int_header("Content-Length"), 2000000)
        self.assertEqual(r.body.count(b"x"), 2000000)
        c.send(req("GET", "/health")); self.assertEqual(c.read_response().body, b"ok\n")
        c.close()
    def test_17b_repeated_large_responses(self):
        c = self.server.conn()
        for _ in range(4):
            c.send(req("GET", "/big/500000"))
            self.assertEqual(len(c.read_response(slow_read=True).body), 500000)
        c.close()

    # 18
    def test_18_client_disconnect(self):
        for raw in (b"GET / HTTP/1.1\r\nHost:", b"POST /echo HTTP/1.1\r\nContent-Length: 1000\r\n\r\nsho",
                    b"GET /big/2000000 HTTP/1.1\r\n\r\n", b"GET / HTTP/1.1\r\nX: "):
            c = self.server.conn(); c.send(raw); c.rst(); c.close()
        time.sleep(0.3)
        c = self.server.conn()
        for _ in range(3):
            c.send(req("GET", "/health")); self.assertEqual(c.read_response().status, 200)
        c.close()
    def test_18b_abandon_large_response(self):
        for _ in range(5):
            c = self.server.conn(); c.send(req("GET", "/big/3000000")); time.sleep(0.02); c.rst(); c.close()
        time.sleep(0.3)
        c = self.server.conn(); c.send(req("GET", "/health"))
        self.assertEqual(c.read_response().body, b"ok\n"); c.close()

    # 24
    def test_24_expect_100_continue(self):
        c = self.server.conn()
        c.send(b"POST /echo HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 6\r\n\r\n")
        first = c._line()
        self.assertEqual(first, "HTTP/1.1 100 Continue")
        self.assertEqual(c._line(), "")
        c.send(b"abcdef")
        r = c.read_response()
        self.assertEqual(r.status, 200); self.assertEqual(r.body, b"abcdef"); c.close()

    # 26
    def test_26_connection_close_route(self):
        c = self.server.conn(); c.send(req("GET", "/close"))
        r = c.read_response(); self.assertEqual(r.status, 200)
        self.assertEqual(r.header("Connection"), "close")
        ok, why = c.expect_closed(); self.assertTrue(ok, why); c.close()


class TestIdleTimeout(unittest.TestCase):
    # 19
    def test_19_idle_timeout_408(self):
        with Server(["--idle-timeout", "1", "--tick-ms", "50"]) as s:
            c = s.conn()
            c.send(req("GET", "/health")); self.assertEqual(c.read_response().status, 200)
            time.sleep(2.3)
            r = c.read_response()
            self.assertEqual(r.status, 408, "expected 408 after idle timeout")
            ok, why = c.expect_closed(); self.assertTrue(ok, why)
            c.close()
    # slow application work is not "idle"
    def test_19b_slow_handler_not_killed_by_idle_timeout(self):
        with Server(["--idle-timeout", "1", "--tick-ms", "50"]) as s:
            c = s.conn(timeout=15)
            c.send(req("GET", "/slow/2500"))
            r = c.read_response()
            self.assertEqual(r.status, 200); self.assertIn(b"slept 2500ms", r.body)
            c.close()
    def test_19b2_half_open_request_times_out(self):
        with Server(["--idle-timeout", "1", "--tick-ms", "50"]) as s:
            c = s.conn(); c.send(b"GET / HTTP/1.1\r\nX: partial")
            time.sleep(2.3)
            self.assertEqual(c.read_response().status, 408)
            c.close()


class TestConcurrency(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server(["--verbose"]); cls.server.start()
    @classmethod
    def tearDownClass(cls):
        sys.stderr.write(cls.server.log()[-1500:] + "\n"); cls.server.stop()

    # 21
    def test_21_concurrent_clients(self):
        errors = []
        def worker(idx):
            try:
                c = self.server.conn()
                for i in range(8):
                    payload = ("client-%d-req-%d" % (idx, i)).encode()
                    c.send(req("POST", "/echo", body=payload))
                    r = c.read_response()
                    if r.status != 200 or r.body != payload:
                        errors.append("client %d req %d -> %r" % (idx, i, r))
                c.close()
            except Exception as exc:
                errors.append("client %d raised %r" % (idx, exc))
        threads = [threading.Thread(target=worker, args=(i,)) for i in range(24)]
        for t in threads: t.start()
        for t in threads: t.join(timeout=60)
        self.assertEqual(errors, [])
        self.assertEqual(self.server.proc.poll(), None, "server died under concurrency")


class TestOverload(unittest.TestCase):
    # 22
    def test_22_worker_queue_overload(self):
        with Server(["--workers", "1", "--queue-size", "2", "--shutdown-timeout", "2"]) as s:
            conns, statuses = [], []
            for _ in range(40):
                c = s.conn(timeout=20); c.send(req("GET", "/slow/300")); conns.append(c)
            for c in conns:
                try:
                    statuses.append(c.read_response().status)
                except Exception as exc:
                    statuses.append(repr(exc))
            for c in conns: c.close()
            ok = statuses.count(200); overloaded = statuses.count(503)
            sys.stderr.write("overload mix: 200=%d 503=%d other=%s\n" % (ok, overloaded, [s for s in statuses if s not in (200, 503)][:4]))
            self.assertGreaterEqual(ok, 1, "at least one request must succeed")
            self.assertGreaterEqual(overloaded, 5, "a full queue must produce 503, got %s" % statuses)
            self.assertEqual(len([x for x in statuses if x not in (200, 503)]), 0,
                             "every client must get a real answer: %s" % statuses)
            self.assertEqual(s.proc.poll(), None, "server must survive overload")
            c = s.conn(); c.send(req("GET", "/health"))
            self.assertEqual(c.read_response().status, 200)
            c.close()
    # 23
    def test_23_max_connections(self):
        with Server(["--max-connections", "3"]) as s:
            conns = [s.conn(timeout=5) for _ in range(6)]
            statuses = []
            for c in conns:
                c.sock.settimeout(2.0)
                try:
                    statuses.append(c.read_response().status)
                except socket.timeout:
                    statuses.append(None)
                except Exception as exc:
                    statuses.append(repr(exc))
            refused = statuses.count(503)
            sys.stderr.write("admission mix: %s\n" % statuses)
            self.assertGreaterEqual(refused, 3, "connections past the limit must get 503")
            for c in conns: c.close()
            c = s.conn(); c.send(req("GET", "/health"))
            self.assertEqual(c.read_response().status, 200)
            c.close()


class TestGracefulShutdown(unittest.TestCase):
    # 20
    def test_20_graceful_shutdown_drains_inflight(self):
        s = Server(["--shutdown-timeout", "5"])
        s.start()
        c = s.conn(timeout=20)
        c.send(req("GET", "/slow/700"))
        time.sleep(0.25)
        s.proc.send_signal(signal.SIGTERM)
        # New clients during the drain get an explicit 503 rather than hanging.
        refused = None
        try:
            c2 = s.conn(timeout=3)
            c2.sock.settimeout(3.0)
            refused = c2.read_response().status
            c2.close()
        except (ConnectionRefusedError, EOFError, socket.timeout, OSError):
            refused = "closed"
        r = c.read_response()
        self.assertEqual(r.status, 200, "in-flight request must complete: %r" % (r,))
        self.assertIn(b"slept 700ms", r.body)
        self.assertIn(refused, (503, "closed"), "new conns during drain: %r" % refused)
        # The drained connection must not be reused for a new request.
        ok, why = c.expect_closed()
        self.assertTrue(ok, "drained connection closes after its response: " + why)
        c.close()
        rc = s.stop()
        self.assertEqual(rc, 0, "clean SIGTERM shutdown must exit 0; log=%s" % s.log())
        self.assertIn("draining", s.log())
    def test_20b_second_signal_forces_exit(self):
        s = Server(["--shutdown-timeout", "30"])
        s.start()
        c = s.conn(timeout=40); c.send(req("GET", "/slow/9000")); time.sleep(0.2)
        s.proc.send_signal(signal.SIGTERM); time.sleep(0.3)
        s.proc.send_signal(signal.SIGINT)
        start = time.time()
        try:
            rc = s.proc.wait(timeout=6)
        except subprocess.TimeoutExpired:
            s.proc.kill(); rc = None
        self.assertEqual(rc, 0, "forced shutdown must still exit 0")
        self.assertLess(time.time() - start, 5.5, "forced shutdown must be prompt")
        c.close()
    def test_20c_sigint_alone_works(self):
        s = Server([])
        s.start()
        rc = s.stop(sig=signal.SIGINT)
        self.assertEqual(rc, 0)


def main():
    global BINARY
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=BINARY)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-k", "--keyword", default=None)
    args, rest = ap.parse_known_args()
    BINARY = os.path.abspath(args.binary)
    if not os.path.exists(BINARY):
        sys.stderr.write("no such binary: %s\n" % BINARY); return 2
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(sys.modules[__name__])
    if args.keyword:
        suite = loader.loadTestsFromNames([n for n in _names(suite) if args.keyword in n], sys.modules[__name__])
    res = unittest.TextTestRunner(verbosity=2 if args.verbose else 1, stream=sys.stderr).run(suite)
    return 0 if res.wasSuccessful() else 1


def _names(suite):
    for t in suite:
        for s in _flatten(t):
            yield s.id()


def _flatten(t):
    if isinstance(t, unittest.TestSuite):
        for s in t:
            for x in _flatten(s):
                yield x
    else:
        yield t


if __name__ == "__main__":
    sys.exit(main())
