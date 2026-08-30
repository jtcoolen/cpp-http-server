cmake_minimum_required(VERSION 3.16)

project(cpp_http_server
  VERSION 0.1.0
  DESCRIPTION "Single-threaded epoll(EPOLLET) event loop + worker thread pool HTTP/1.1 server"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# This server is Linux specific on purpose: it uses epoll, accept4(), eventfd()
# and signalfd().  Fail loudly instead of building something that cannot work.
# ---------------------------------------------------------------------------
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "This project targets Linux (epoll/EPOLLET, accept4, eventfd, signalfd). "
    "Current system: ${CMAKE_SYSTEM_NAME}. Build it on Linux.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CXX_FLAGS)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(http_server
  src/main.cpp
  src/config.cpp
  src/http_common.cpp
  src/http_parser.cpp
  src/handler.cpp
  src/connection.cpp
  src/event_loop.cpp
  src/thread_pool.cpp)

target_include_directories(http_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(http_server PRIVATE cxx_std_20)
target_compile_definitions(http_server PRIVATE _GNU_SOURCE)

target_compile_options(http_server PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wold-style-cast
  -Wnon-virtual-dtor
  -Wcast-qual
  -Wstrict-aliasing=2)

target_link_libraries(http_server PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Optional sanitiser builds.  These are what the lifetime / data-race review was
# validated with:
#   cmake -B build-asan -DENABLE_ASAN=ON  && cmake --build build-asan
#   cmake -B build-tsan -DENABLE_TSAN=ON  && cmake --build build-tsan
# ASan and TSan cannot be combined in one binary.
# ---------------------------------------------------------------------------
option(ENABLE_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive")
endif()

if(ENABLE_ASAN)
  target_compile_options(http_server PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(http_server PRIVATE -fsanitize=address,undefined)
endif()

if(ENABLE_TSAN)
  target_compile_options(http_server PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g -O1)
  target_link_options(http_server PRIVATE -fsanitize=thread)
endif()

option(ENABLE_OSS_FUZZ_STYLE_UNIT_TESTS "Build the small parser unit test binary" ON)
if(ENABLE_OSS_FUZZ_STYLE_UNIT_TESTS)
  add_executable(parser_tests tests/parser_tests.cpp src/http_parser.cpp src/http_common.cpp src/config.cpp)
  target_include_directories(parser_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(parser_tests PRIVATE cxx_std_20)
  target_compile_definitions(parser_tests PRIVATE _GNU_SOURCE)
  target_compile_options(parser_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(parser_tests PRIVATE Threads::Threads)

  enable_testing()
  add_test(NAME parser_tests COMMAND parser_tests)
endif()
