#pragma once
//
// The application.  handle_request() runs on worker threads only: it may block
// (that is the whole point of the pool) but it must not touch epoll, sockets or
// Connection state.  It gets an immutable Request and returns an owned
// Response, which is then posted back to the event loop.
//

#include "common.h"
#include "config.h"

#include <string>

namespace http {

Response handle_request(const Request& req, const Config& cfg);

} // namespace http
