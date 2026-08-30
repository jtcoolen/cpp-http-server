//
// Parser unit tests.  These run without sockets and feed input in small pieces
// so that every incremental path (split request lines, split headers, split
// chunk-size lines, split chunk data) is exercised, not just the easy
// "everything in one buffer" case.
//
// Build:  cmake --build build -- target parser_tests && ./build/parser_tests
//

#include "http_parser.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace http;

namespace {

int g_checks = 0;
int g_failures = 0;
std::string g_context;

void check(bool ok, std::string_view what) {
  ++g_checks;
  if (ok)
    return;
  ++g_failures;
  std::fprintf(stderr, "FAIL [%s]: %.*s\n", g_context.c_str(), static_cast<int>(what.size()), what.data());
}

void check_eq(long long got, long long want, std::string_view what) {
  ++g_checks;
  if (got == want)
      return;
  ++g_failures;
  std::fprintf(stderr, "FAIL [%s]: %.*s (got %lld, want %lld)\n", g_context.c_str(),
               static_cast<int>(what.size()), what.data(), got, want);
}

struct ParserLimits small_limits() {
  ParserLimits l;
  l.max_request_line_bytes = 128;
  l.max_header_bytes = 256;
  l.max_body_bytes = 1024;
  l.max_request_bytes = 4096;
  return l;
}

struct Outcome {
  ParseStatus status = ParseStatus::kNeedMore;
  Request req;
  std::string err;
  int continue_sent = 0;
};

// Feeds `data` to the parser `piece` bytes at a time.  Returns the final status.
Outcome run(std::string_view data, std::size_t piece, const ParserLimits& limits) {
  HttpRequestParser parser(limits);
  ByteBuf buf;
  Outcome out;
  std::size_t pos = 0;
  int guard = 0;
  while (true) {
    if (++guard > 100000) {
      out.err = "test guard tripped (parser made no progress)";
      out.status = ParseStatus::kBadRequest;
      return out;
    }
    // Ask the parser first: it may already be done with buffered bytes.
    Request req;
    std::string err;
    const ParseStatus st = parser.parse(buf, req, err);
    if (st == ParseStatus::kRequest) {
      out.status = st;
      out.req = std::move(req);
      out.err = err;
      return out;
    }
    if (st == ParseStatus::kExpect100) {
      ++out.continue_sent;
      out.status = st; // remember, then keep going
      continue;
    }
    if (st == ParseStatus::kBadRequest || st == ParseStatus::kTooLarge) {
      out.status = st;
      out.err = err;
      return out;
    }
    if (pos >= data.size()) {
      out.status = ParseStatus::kNeedMore;
      out.err = err;
      return out;
    }
    const std::size_t take = std::min(piece, data.size() - pos);
    buf.append(data.data() + pos, take);
    pos += take;
  }
}

Outcome run_once(std::string_view data) {
  return run(data, data.size(), small_limits());
}

// ---------------------------------------------------------------------------

void test_simple_get() {
  g_context = "simple get";
  auto o = run_once("GET /hello?x=1 HTTP/1.1\r\nHost: example\r\n\r\n");
  check(o.status == ParseStatus::kRequest, "status == kRequest");
  check(o.req.method == Method::Get, "method GET");
  check(o.req.target == "/hello?x=1", "target");
  check_eq(o.req.minor, 1, "version 1.1");
  check(o.req.keep_alive, "1.1 keep-alive by default");
  check_eq(static_cast<long long>(o.req.headers.size()), 1, "one header");
  check(o.req.header("host") == "example", "case-insensitive header lookup");
  check(o.req.body.empty(), "no body");
}

void test_fragmented() {
  g_context = "fragmented";
  for (std::size_t piece : {std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(7)}) {
    g_context = "fragmented piece=" + std::to_string(piece);
    auto o = run("POST /e HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello", piece, small_limits());
    check(o.status == ParseStatus::kRequest, "request completed");
    check(o.req.body == "hello", "body reassembled");
    check_eq(static_cast<long long>(o.req.total_bytes), 44, "total_bytes");
  }
}

void test_bare_lf_rejected() {
  g_context = "bare lf";
  check(run_once("GET / HTTP/1.1\nHost: x\n\n").status == ParseStatus::kBadRequest, "bare LF in headers");
  check(run_once("GET / HTTP/1.1\n\r\n").status == ParseStatus::kBadRequest, "bare LF in request line");
}

void test_absolute_form_rejected() {
  g_context = "absolute form";
  check(run_once("GET http://example.com/ HTTP/1.1\r\n\r\n").status == ParseStatus::kBadRequest,
        "absolute-form rejected");
  check(run_once("GET //example.com/ HTTP/1.1\r\n\r\n").status == ParseStatus::kBadRequest,
        "protocol-relative rejected");
  check(run_once("GET example.com:80/ HTTP/1.1\r\n\r\n").status == ParseStatus::kBadRequest,
        "authority-form rejected");
  check(run_once("CONNECT example.com:80 HTTP/1.1\r\n\r\n").status == ParseStatus::kBadRequest,
        "connect-form rejected");
  check(run_once("GET /a#frag HTTP/1.1\r\n\r\n").status == ParseStatus::kBadRequest, "fragment rejected");
}

void test_versions() {
  g_context = "versions";
  auto o10 = run_once("GET / HTTP/1.0\r\n\r\n");
  check(o10.status == ParseStatus::kRequest, "1.0 parses");
  check(!o10.req.keep_alive, "1.0 closes by default");
  auto ka = run_once("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
  check(ka.status == ParseStatus::kRequest, "1.0 + keep-alive parses");
  check(ka.req.keep_alive, "1.0 keep-alive honoured");
  auto close11 = run_once("GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
  check(!close11.req.keep_alive, "1.1 + Connection: close");
  check(run_once("GET / HTTP/1.2\r\n\r\n").status == ParseStatus::kBadRequest, "1.2 rejected");
  check(run_once("GET / HTTP/0.9\r\n\r\n").status == ParseStatus::kBadRequest, "0.9 rejected");
  check(run_once("GET / HTTP/1.1\r\nConnection: upgrade\r\n\r\n").status == ParseStatus::kBadRequest,
        "Connection: upgrade rejected");
}

void test_extension_method() {
  g_context = "extension method";
  auto o = run_once("PUT /x HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi");
  check(o.status == ParseStatus::kRequest, "PUT parses (405 is an app decision)");
  check(o.req.method == Method::Unknown, "PUT is Unknown to the parser");
  check(o.req.body == "hi", "PUT body framed");
  check(o.req.method_name == "PUT", "method name preserved");
}

void test_options_star() {
  g_context = "options star";
  auto o = run_once("OPTIONS * HTTP/1.1\r\n\r\n");
  check(o.status == ParseStatus::kRequest, "OPTIONS * parses");
  check(o.req.is_star_target(), "star target");
  check(run_once("GET * HTTP/1.1\r\n\r\n").status == ParseStatus::kBadRequest, "GET * rejected");
}

void test_content_length_rules() {
  g_context = "content-length";
  check(run_once("POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc")
            .status == ParseStatus::kBadRequest,
        "duplicate Content-Length rejected even when identical");
  check(run_once("POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc")
            .status == ParseStatus::kBadRequest,
        "conflicting Content-Length rejected");
  check(run_once("POST / HTTP/1.1\r\nContent-Length: 3, 3\r\n\r\nabc").status == ParseStatus::kBadRequest,
        "list-valued Content-Length rejected");
  check(run_once("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n").status == ParseStatus::kBadRequest,
        "non-numeric Content-Length rejected");
  check(run_once("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n").status == ParseStatus::kBadRequest,
        "negative Content-Length rejected");
  check(run_once("POST / HTTP/1.1\r\nContent-Length: 007\r\n\r\nabc").status == ParseStatus::kBadRequest,
        "leading zeros rejected");
  check(run_once("POST / HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n").status == ParseStatus::kTooLarge,
        "oversized Content-Length -> too large");
  auto zero = run_once("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
  check(zero.status == ParseStatus::kRequest, "Content-Length: 0 is a complete request");
  check(zero.req.body.empty(), "empty body");
}

void test_framing_conflicts() {
  g_context = "framing conflicts";
  check(run_once("POST / HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
            .status == ParseStatus::kBadRequest,
        "Content-Length + chunked rejected");
  check(run_once("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "0\r\n\r\n")
            .status == ParseStatus::kBadRequest,
        "duplicate Transfer-Encoding rejected");
  check(run_once("POST / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n")
            .status == ParseStatus::kBadRequest,
        "unsupported transfer coding rejected");
  check(run_once("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
            .status == ParseStatus::kBadRequest,
        "chunked on HTTP/1.0 rejected");
}

void test_chunked() {
  g_context = "chunked";
  auto o = run_once("POST /e HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "5\r\nhello\r\n6;ext=1\r\nworld!\r\n0\r\n\r\n");
  check(o.status == ParseStatus::kRequest, "chunked request completes");
  check(o.req.body == "helloworld!", "chunked body decoded");

  auto with_trailers = run_once("POST /e HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "3\r\nabc\r\n0\r\nX-Trail: 1\r\n\r\n");
  check(with_trailers.status == ParseStatus::kRequest, "trailers accepted");
  check(with_trailers.req.body == "abc", "trailer body");

  for (std::size_t piece : {std::size_t(1), std::size_t(2), std::size_t(4), std::size_t(5)}) {
    g_context = "chunked piece=" + std::to_string(piece);
    auto f = run("POST /e HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "a\r\n0123456789\r\n3\r\nabc\r\n0\r\n\r\n",
                 piece, small_limits());
    check(f.status == ParseStatus::kRequest, "fragmented chunked completes");
    check(f.req.body == "0123456789abc", "fragmented chunked body");
  }

  // Valid stream (data, CRLF, last chunk): must parse.
  check(run_once("POST /e HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n")
            .status == ParseStatus::kRequest,
        "minimal chunked stream accepted");
  // Chunk data followed by junk instead of CRLF.
  check(run_once("POST /e HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "5\r\nhelloJUNK\r\n0\r\n\r\n")
            .status == ParseStatus::kBadRequest,
        "missing CRLF after chunk data rejected");
  check(run_once("POST /e HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nz\r\nx\r\n0\r\n\r\n")
            .status == ParseStatus::kBadRequest,
        "bad chunk size rejected");
  check(run_once("POST /e HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "800\r\naaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n0\r\n\r\n")
            .status == ParseStatus::kTooLarge,
        "chunked body over the limit rejected (declared chunk size)");
}

void test_header_validation() {
  g_context = "headers";
  check(run_once("GET / HTTP/1.1\r\nBad Header: x\r\n\r\n").status == ParseStatus::kBadRequest,
        "space in header name rejected");
  check(run_once("GET / HTTP/1.1\r\n:x\r\n\r\n").status == ParseStatus::kBadRequest,
        "empty header name rejected");
  check(run_once("GET / HTTP/1.1\r\nNoColon\r\n\r\n").status == ParseStatus::kBadRequest,
        "header line without colon rejected");
  check(run_once("GET / HTTP/1.1\r\nX: a\r\n continued\r\n\r\n").status == ParseStatus::kBadRequest,
        "obs-fold rejected");
  check(run_once("GET / HTTP/1.1\r\nX\x01: a\r\n\r\n").status == ParseStatus::kBadRequest,
        "control char in header name rejected");
  check(run_once("GET / HTTP/1.1\r\nX: a\x01b\r\n\r\n").status == ParseStatus::kBadRequest,
        "control char in header value rejected");
  // A CR that is not part of CRLF: the value contains a lone CR.
  check(run_once("GET / HTTP/1.1\r\nX: a\rb\r\n\r\n").status == ParseStatus::kBadRequest,
        "lone CR in header value rejected");
  check(run_once("GET / HTTP/1.1\r\nX: injected\r\nEvil: 1\r\n\r\n").status == ParseStatus::kRequest,
        "two well-formed headers are just two headers");
  auto o = run_once("GET / HTTP/1.1\r\nX:a\r\n\r\n");
  check(o.status == ParseStatus::kRequest && o.req.header("x") == "a", "no-space value ok");
}

void test_size_limits() {
  g_context = "limits";
  const std::string long_line = "GET /" + std::string(300, 'a') + " HTTP/1.1\r\n\r\n";
  check(run(long_line, 64, small_limits()).status == ParseStatus::kTooLarge, "request line limit");

  std::string many_headers = "GET / HTTP/1.1\r\n";
  for (int i = 0; i < 40; ++i)
    many_headers += "X-Header-" + std::to_string(i) + ": value-value-value\r\n";
  many_headers += "\r\n";
  check(run(many_headers, 64, small_limits()).status == ParseStatus::kTooLarge, "header block limit");

  check(run("POST / HTTP/1.1\r\nContent-Length: 1024\r\n\r\n" + std::string(1024, 'a'), 64,
            small_limits())
            .status == ParseStatus::kRequest,
        "body exactly at the limit is accepted");
  check(run("POST / HTTP/1.1\r\nContent-Length: 1025\r\n\r\n" + std::string(1025, 'a'), 64,
            small_limits())
            .status == ParseStatus::kTooLarge,
        "body over the body limit is rejected");
}

void test_expect_100() {
  g_context = "expect 100";
  auto o = run("POST /e HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\nhi", 8,
               small_limits());
  check(o.status == ParseStatus::kRequest, "request completes after 100-continue");
  check_eq(o.continue_sent, 1, "exactly one 100 Continue");
  check(o.req.body == "hi", "body after interim response");
  check(o.req.expect_100_continue, "flag recorded");
  check(run_once("POST /e HTTP/1.1\r\nExpect: 261-ntto\r\nContent-Length: 2\r\n\r\nhi")
            .status == ParseStatus::kBadRequest,
        "unknown Expect rejected");
}

void test_no_overread() {
  g_context = "no overread";
  // The parser must stop exactly at the end of the request and leave pipelined
  // bytes untouched in the buffer.
  HttpRequestParser parser(small_limits());
  ByteBuf buf;
  buf.append("GET /one HTTP/1.1\r\n\r\nGET /two HTTP/1.1\r\n\r\n");
  Request a;
  std::string err;
  check(parser.parse(buf, a, err) == ParseStatus::kRequest, "first request");
  check(a.target == "/one", "first target");
  check(buf.size() == std::string_view("GET /two HTTP/1.1\r\n\r\n").size(), "second request still buffered");
  Request b;
  check(parser.parse(buf, b, err) == ParseStatus::kRequest, "second request");
  check(b.target == "/two", "second target");
  check(buf.empty(), "buffer drained");
}

} // namespace

int main() {
  test_simple_get();
  test_fragmented();
  test_bare_lf_rejected();
  test_absolute_form_rejected();
  test_versions();
  test_extension_method();
  test_options_star();
  test_content_length_rules();
  test_framing_conflicts();
  test_chunked();
  test_header_validation();
  test_size_limits();
  test_expect_100();
  test_no_overread();

  std::fprintf(stderr, "parser_tests: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
