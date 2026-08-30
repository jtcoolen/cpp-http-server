#include "config.h"

#include <getopt.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <sstream>
#include <thread>

namespace http {

namespace {

void usage(std::ostream& os, const Config& cfg);

bool parse_size(const char* s, std::size_t& out) {
  if (s == nullptr || *s == '\0')
    return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0')
    return false;
  out = static_cast<std::size_t>(v);
  return true;
}

bool parse_double(const char* s, double& out) {
  if (s == nullptr || *s == '\0')
    return false;
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(s, &end);
  if (errno != 0 || end == s || *end != '\0')
    return false;
  out = v;
  return true;
}

bool parse_port(const char* s, std::uint16_t& out) {
  std::size_t v = 0;
  if (!parse_size(s, v) || v > 65535)
    return false;
  out = static_cast<std::uint16_t>(v);
  return true;
}

} // namespace

std::string Config::describe() const {
  std::ostringstream os;
  os << "bind=" << bind << ":" << port << " workers=" << workers << " queue=" << max_queue
     << " max_conn=" << max_connections << " idle=" << idle_timeout_sec << "s"
     << " shutdown=" << shutdown_timeout_sec << "s" << " max_req=" << max_request_bytes
     << "B max_headers=" << max_header_bytes << "B max_body=" << max_body_bytes
     << "B max_out=" << max_output_bytes << "B";
  return os.str();
}

bool parse_args(int argc, char** argv, Config& cfg, std::string& err, bool& wants_exit_success) {
  const std::size_t hw = std::thread::hardware_concurrency();
  cfg.workers = hw > 0 ? std::min<std::size_t>(hw, 8) : 4;

  static constexpr option opts[] = {
      {"bind", required_argument, nullptr, 'b'},
      {"port", required_argument, nullptr, 'p'},
      {"port-file", required_argument, nullptr, 1},
      {"backlog", required_argument, nullptr, 2},
      {"workers", required_argument, nullptr, 'w'},
      {"queue-size", required_argument, nullptr, 3},
      {"max-connections", required_argument, nullptr, 4},
      {"max-request-line", required_argument, nullptr, 5},
      {"max-headers", required_argument, nullptr, 6},
      {"max-body", required_argument, nullptr, 7},
      {"max-request", required_argument, nullptr, 8},
      {"max-output", required_argument, nullptr, 9},
      {"max-response", required_argument, nullptr, 10},
      {"idle-timeout", required_argument, nullptr, 11},
      {"shutdown-timeout", required_argument, nullptr, 12},
      {"tick-ms", required_argument, nullptr, 13},
      {"verbose", no_argument, nullptr, 'v'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  std::ostringstream bad;
  bool failed = false;
  auto fail = [&](const char* opt, const char* value) {
    bad << "invalid value for " << opt << ": '" << (value ? value : "") << "'\n";
    failed = true;
  };

  int c = -1;
  while ((c = ::getopt_long(argc, argv, "b:p:w:vh", opts, nullptr)) != -1) {
    switch (c) {
    case 'b':
      cfg.bind = optarg;
      break;
    case 'p':
      if (!parse_port(optarg, cfg.port))
        fail("--port", optarg);
      break;
    case 1:
      cfg.port_file = optarg;
      break;
    case 2: {
      std::size_t v = 0;
      if (!parse_size(optarg, v) || v == 0 || v > 65535)
        fail("--backlog", optarg);
      else
        cfg.backlog = static_cast<int>(v);
      break;
    }
    case 'w':
      if (!parse_size(optarg, cfg.workers) || cfg.workers == 0)
        fail("--workers", optarg);
      break;
    case 3:
      if (!parse_size(optarg, cfg.max_queue) || cfg.max_queue == 0)
        fail("--queue-size", optarg);
      break;
    case 4:
      if (!parse_size(optarg, cfg.max_connections) || cfg.max_connections == 0)
        fail("--max-connections", optarg);
      break;
    case 5:
      if (!parse_size(optarg, cfg.max_request_line_bytes) || cfg.max_request_line_bytes < 16)
        fail("--max-request-line", optarg);
      break;
    case 6:
      if (!parse_size(optarg, cfg.max_header_bytes) || cfg.max_header_bytes < 64)
        fail("--max-headers", optarg);
      break;
    case 7:
      if (!parse_size(optarg, cfg.max_body_bytes))
        fail("--max-body", optarg);
      break;
    case 8:
      if (!parse_size(optarg, cfg.max_request_bytes))
        fail("--max-request", optarg);
      break;
    case 9:
      if (!parse_size(optarg, cfg.max_output_bytes) || cfg.max_output_bytes < 1024)
        fail("--max-output", optarg);
      break;
    case 10:
      if (!parse_size(optarg, cfg.max_response_bytes) || cfg.max_response_bytes < 1)
        fail("--max-response", optarg);
      break;
    case 11:
      if (!parse_double(optarg, cfg.idle_timeout_sec) || cfg.idle_timeout_sec < 0.0)
        fail("--idle-timeout", optarg);
      break;
    case 12:
      if (!parse_double(optarg, cfg.shutdown_timeout_sec) || cfg.shutdown_timeout_sec < 0.0)
        fail("--shutdown-timeout", optarg);
      break;
    case 13: {
      std::size_t v = 0;
      if (!parse_size(optarg, v) || v < 10 || v > 5000)
        fail("--tick-ms", optarg);
      else
        cfg.tick_ms = static_cast<int>(v);
      break;
    }
    case 'v':
      cfg.verbose = true;
      break;
    case 'h': {
      std::ostringstream os;
      usage(os, cfg);
      err = os.str();
      wants_exit_success = true;
      return false;
    }
    default:
      bad << "unrecognised option (see --help)\n";
      failed = true;
      break;
    }
  }

  if (optind < argc) {
    bad << "unexpected positional argument: '" << argv[optind] << "'\n";
    failed = true;
  }

  if (cfg.max_request_bytes < cfg.max_body_bytes + cfg.max_header_bytes)
    cfg.max_request_bytes = cfg.max_body_bytes + cfg.max_header_bytes;

  if (failed) {
    std::ostringstream os;
    usage(os, cfg);
    err = bad.str() + os.str();
    return false;
  }
  return true;
}

namespace {

void usage(std::ostream& os, const Config& cfg) {
  os << "usage: http_server [options]\n"
     << "  -b, --bind <addr>        bind address (default \"::\" = dual-stack IPv6+IPv4)\n"
     << "  -p, --port <port>        port, 0 = ephemeral (default 8080)\n"
     << "      --port-file <path>   write the actual bound port to <path>\n"
     << "      --backlog <n>        listen backlog (default 1024)\n"
     << "  -w, --workers <n>        worker threads (default min(nproc,8)=" << cfg.workers << ")\n"
     << "      --queue-size <n>     bounded worker queue (default 1024)\n"
     << "      --max-connections <n> concurrent connection limit (default 4096)\n"
     << "      --max-request-line <bytes>  (default 8192)\n"
     << "      --max-headers <bytes>       (default 65536)\n"
     << "      --max-body <bytes>          (default 8388608)\n"
     << "      --max-request <bytes>       line+headers+body cap (auto-raised)\n"
     << "      --max-output <bytes>        per-connection output cap (default 16MiB)\n"
     << "      --max-response <bytes>      handler body cap (default 8MiB)\n"
     << "      --idle-timeout <sec>        408 after this much client silence (default 15)\n"
     << "      --shutdown-timeout <sec>    graceful drain budget (default 5)\n"
     << "      --tick-ms <ms>              timeout scan interval (default 200)\n"
     << "  -v, --verbose            log per-request and per-connection detail\n"
     << "  -h, --help               this text\n";
}

} // namespace

} // namespace http
