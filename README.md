# bnio

**bnio** is a modern C++20 async I/O library. It provides a
scheduler-based async model — every I/O operation is a lazy sender that composes
with the standard receiver pattern. TCP, TLS (OpenSSL), DNS resolution, timers,
and composed writes all ship out of the box.

> **Status:** Linux uses io_uring; macOS and BSD use kqueue. Both backends expose
> the same high-level `io_context`, TCP, UDP, DNS, timer, and TLS interfaces.
> OpenSSL ≥ 1.1 is required for TLS support.

---

## Table of Contents

- [bnio](#bnio)
  - [Table of Contents](#table-of-contents)
  - [Dependencies](#dependencies)
  - [Quick Start](#quick-start)
    - [Minimal Example](#minimal-example)
  - [Examples](#examples)
    - [mini\_curl](#mini_curl)
      - [Usage](#usage)
      - [Options](#options)
      - [Design](#design)
    - [Low-level base examples](#low-level-base-examples)
    - [Raw TCP echo](#raw-tcp-echo)
    - [Standalone Asio echo](#standalone-asio-echo)
  - [Architecture](#architecture)
    - [Read and write semantics](#read-and-write-semantics)
  - [Build Options](#build-options)
    - [GoogleTest dependency](#googletest-dependency)
    - [bexec dependency providers](#bexec-dependency-providers)
    - [Using a local bexec checkout](#using-a-local-bexec-checkout)
    - [Shared library build](#shared-library-build)
    - [Use bnio as a dependency](#use-bnio-as-a-dependency)
      - [Installed CMake package](#installed-cmake-package)
      - [pkg-config](#pkg-config)
      - [Source tree](#source-tree)
    - [Coverage report](#coverage-report)
  - [License](#license)

---

## Dependencies

| Dependency    | Minimum Version | Notes                                |
| ------------- | --------------- | ------------------------------------ |
| C++ compiler  | C++20           | GCC 12+ or Clang 16+                 |
| CMake         | 3.20            |                                      |
| liburing      | 2.1             | Linux only; `pkg-config` required    |
| OpenSSL       | 1.1             | `pkg-config` required; TLS feature   |
| bexec         | 0.0.1           | Package, source tree, or FetchContent |

---

## Quick Start

```sh
# Configure & build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure
```

### Minimal Example

```cpp
#include <bnio/bnio.h>

int main() {
  bnio::base::ring ring;
  if (ring.queue_init(8) < 0) {
    return 1;
  }

  bnio::base::submission_queue_entry sqe = ring.get_sqe();
  if (sqe.raw() == nullptr) {
    return 1;
  }

  sqe.prep_nop();
  sqe.set_data64(42);

  if (ring.submit() < 0) {
    return 1;
  }

  bnio::base::completion_queue_entry cqe;
  if (ring.wait_cqe(cqe) < 0) {
    return 1;
  }

  const int result = cqe.res();
  ring.cqe_seen(cqe);
  return result;
}
```

---

## Examples

### mini_curl

`examples/mini_curl/` contains a compact HTTP/HTTPS client that demonstrates the
full async stack: DNS resolution, TCP connect with fallback, TLS handshake,
HTTP request/response, and graceful shutdown — all via the scheduler API.

#### Usage

```sh
# HTTP GET
bnio_mini_curl http://httpbin.org/get

# HTTPS GET (auto-detected from https:// scheme)
bnio_mini_curl https://httpbin.org/get

# POST JSON data
bnio_mini_curl -X POST -H "Content-Type: application/json" \
  -d '{"key":"value"}' https://httpbin.org/post

# Download to file, follow redirects
bnio_mini_curl -L -o output.html https://example.com

# HEAD request with verbose progress
bnio_mini_curl -I -v https://httpbin.org/status/200

# IPv4-only resolution, skip TLS verification
bnio_mini_curl -k --ipv4 https://localhost:8443/test
```

#### Options

| Flag   | Long form            | Description                                    |
| ------ | -------------------- | ---------------------------------------------- |
| `-X`   | `--request METHOD`   | HTTP method (default: `GET`)                   |
| `-I`   | `--head`             | Send a HEAD request                            |
| `-H`   | `--header HEADER`    | Add a request header (`Name: Value`)           |
| `-d`   | `--data DATA`        | Send POST data in the request body             |
| `-L`   | `--location`         | Follow redirects (up to 10)                    |
| `-o`   | `--output FILE`      | Write response body to file instead of stdout  |
| `-k`   | `--insecure`         | Skip TLS certificate verification              |
| `-v`   | `--verbose`          | Print connection progress to stderr            |
|        | `--host HOST`        | Override the URL host                          |
|        | `--port PORT`        | Override the URL port/service                  |
|        | `--path PATH`        | Override the request path                      |
|        | `--ipv4`             | Resolve only IPv4 addresses                    |
|        | `--ipv6`             | Resolve only IPv6 addresses                    |

#### Design

The client is split into several files under `examples/mini_curl/`:

- [`mini_curl.cpp`](examples/mini_curl/mini_curl.cpp) — `main()`, argument
  parsing, and URL parsing.
- [`request.cpp`](examples/mini_curl/request.cpp) — HTTP request construction.
- [`mini_curl/request.hpp`](examples/mini_curl/mini_curl/request.hpp) — request
  type definition.
- [`mini_curl/client.hpp`](examples/mini_curl/mini_curl/client.hpp) — the
  async client class with sender/receiver plumbing, DNS→connect→TLS→HTTP
  pipeline, and redirect following.
- [`mini_curl/client_connection.hpp`](examples/mini_curl/mini_curl/client_connection.hpp) —
  connection establishment with endpoint fallback.
- [`mini_curl/client_transfer.hpp`](examples/mini_curl/mini_curl/client_transfer.hpp) —
  HTTP request/response transfer.
- [`mini_curl/client_receivers.hpp`](examples/mini_curl/mini_curl/client_receivers.hpp) —
  completion receivers.
- [`mini_curl/client_redirect.hpp`](examples/mini_curl/mini_curl/client_redirect.hpp) —
  redirect handling.
- [`mini_curl/client_output.hpp`](examples/mini_curl/mini_curl/client_output.hpp) —
  response body output.
- [`mini_curl/operation_registry.hpp`](examples/mini_curl/mini_curl/operation_registry.hpp) —
  operation lifetime container.

Key patterns demonstrated:

- **Sender/receiver operation lifecycle** — every async step (resolve, connect,
  handshake, write, read, shutdown) is a sender connected to a receiver,
  managed by an `operation_registry`.
- **Endpoint fallback** — resolved endpoints are tried sequentially;
  connection failures advance to the next endpoint.
- **TLS handshake integration** — after TCP connect, the socket is moved into
  an `ssl_stream`; handshake, encrypted read/write, and shutdown all flow
  through the same scheduler API.
- **Write-all semantics** — `async_write()` retries short writes until the
  whole buffer is accepted; `async_write_some()` is available when callers want
  one native write attempt.
- **Redirect following** — response headers are buffered until `\r\n\r\n`;
  3xx responses with a `Location` header trigger a new request (method changed
  to GET per RFC 7231).
- **Graceful TLS shutdown** — `SSL_ERROR_ZERO_RETURN` from the server is
  treated as a clean close; the client sends `close_notify` in response.

### Low-level base examples

`examples/base/linux/` contains small, direct programs that demonstrate the
`bnio::base` thin wrapper around `liburing`:

| Executable                    | What it does                                              |
| ----------------------------- | --------------------------------------------------------- |
| `bnio_base_probe`             | Print kernel opcode support                               |
| `bnio_base_nop`               | Smallest possible operation with `user_data` validation   |
| `bnio_base_timeout`           | Kernel timer completions                                  |
| `bnio_base_poll`              | Wait for pipe readiness                                   |
| `bnio_base_echo_server`       | Echo server event loop (accept → recv → send)             |

```sh
# Run any example
./build/examples/base/linux/bnio_base_probe
./build/examples/base/linux/bnio_base_echo_server  # starts on port 7000
```

### TCP throughput benchmark

`benchmarks/throughput/` contains functionally equivalent bnio and
standalone-Asio TCP echo servers plus one shared load client. It exercises the
sender/receiver operation lifecycle and uses `ctx.run()` as the bnio server's
event loop.

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DBNIO_BUILD_BENCHMARKS=ON
cmake --build build-bench --target bnio_throughput_benchmark \
  asio_throughput_benchmark throughput_benchmark_client
scripts/benchmark.sh --build-dir build-bench
```

### Standalone Asio echo

`examples/asio_echo/` contains an equivalent HTTP/1.1 echo server written
against **standalone Asio** (no Boost, no Beast). Disabled by default.

```sh
cmake -S . -B build-asio -DBNIO_BUILD_ASIO_EXAMPLES=ON
cmake --build build-asio --target bnio_asio_echo_server
./build-asio/examples/asio_echo/bnio_asio_echo_server [port]
```

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│  bnio::io_context  (event loop + scheduler)     │
│  bnio::ssl_stream  (TLS over any next layer)     │
│  bnio::tcp::{socket, acceptor} / udp::socket     │
├─────────────────────────────────────────────────┤
│  bnio::async_io  (non-owning views + DNS)        │
│  buffer/descriptor/stream/datagram socket views  │
│  Linux: io_uring_context  BSD: kqueue_context    │
├─────────────────────────────────────────────────┤
│  bnio::base  (thin system call wrappers)         │
│  Linux: ring  submission_queue_entry             │
│         completion_queue_entry                   │
│  BSD:   kqueue  event  event_list_view           │
└─────────────────────────────────────────────────┘
```text

- **`bnio::base`** — the thinnest possible wrapper around system call APIs.
  Linux: owns the ring fd, exposes SQE preparation and CQE walking. BSD: owns a
  kqueue fd, exposes kevent registration and polling.
- **`bnio::async_io`** — platform-neutral vocabulary types. Non-owning views
  for descriptors, buffers, socket views, IP addresses/endpoints, and DNS
  queries. This layer intentionally has no RAII owners.
  `linux_native::io_uring_context` and `bsd_native::kqueue_context` provide the
  platform-level event loop selected by the target system.
- **`bnio::io_context`** — the high-level async runtime. Uses
  `concurrency_hint` to allocate native run-loop slots; each thread entering
  `run()` claims a slot with its own native context. Produces schedulers
  (dispatch and post semantics), and provides sender factories for socket views,
  descriptors, DNS, polling, and timers. Stream owners build their higher-level
  senders on top.
- **`bnio::ssl_stream`** — an RAII TLS stream that layers over any next layer
  (default: `tcp_socket`). Owns the SSL object and transport BIO pairs. Senders
  for handshake, read, write, and shutdown are produced by the stream.

### Scheduler model

```cpp
bnio::io_context ctx;

// Post scheduler: schedule() always posts through the event loop.
auto sched = ctx.get_post_scheduler();

// Every async operation is a sender:
//   scheduler.async_resolve(query, result)
//   socket.async_connect(scheduler, endpoint)
//   scheduler.async_poll(descriptor, poll_mask)
//   scheduler.async_read(descriptor, buffer, offset)
//   scheduler.async_read_some(descriptor, buffer, offset)
//   scheduler.async_write(descriptor, buffer, offset)
//   scheduler.async_write_some(descriptor, buffer, offset)
//   ssl_stream.async_handshake(scheduler, type)
//   ssl_stream.async_write(scheduler, buffer)
//   ssl_stream.async_write_some(scheduler, buffer)
//   ssl_stream.async_read(scheduler, buffer)
//   ssl_stream.async_read_some(scheduler, buffer)
//   ssl_stream.async_shutdown(scheduler)

spawn(socket.async_connect(sched, endpoint), my_receiver{});
ctx.run();
```

### Read and write semantics

`async_read()` reads one available chunk and completes with the number of bytes
received. It may complete with fewer bytes than the buffer size, so protocol
parsers should keep calling it until they have enough data. `async_read_some()`
is the explicit spelling for the same one-read operation.

`async_write()` is a write-all operation for TCP streams, TLS streams, and file
descriptors: it repeats bounded native writes until the whole buffer has been
transferred or an error/stopped signal occurs. `async_write_some()` performs one
bounded write attempt and returns that attempt's byte count.

---

## Build Options

| Option                         | Default      | Description                                  |
| ------------------------------ | ------------ | -------------------------------------------- |
| `BUILD_SHARED_LIBS`            | `OFF`        | Build `bnio` as a shared library             |
| `BNIO_BUILD_TESTS`             | top-level    | Build GoogleTest tests and enable CTest      |
| `BNIO_BUILD_EXAMPLES`          | top-level    | Build example executables                    |
| `BNIO_BUILD_BENCHMARKS`        | `OFF`        | Build bnio/Asio benchmark pairs and fetch Asio |
| `BNIO_BUILD_ASIO_EXAMPLES`     | `OFF`        | Build Asio examples and fetch Asio           |
| `BNIO_INSTALL`                 | top-level    | Generate installation and package files      |
| `BNIO_ENABLE_COVERAGE`         | `OFF`        | Instrument GCC/Clang builds for coverage     |
| `BNIO_GOOGLETEST_PROVIDER`     | `AUTO`       | `AUTO`, `FIND_PACKAGE`, `FETCH` (tests only)  |
| `BNIO_GOOGLETEST_GIT_TAG`      | `v1.17.0`    | Git ref used by the test-only `FETCH` provider |
| `BNIO_BEXEC_PROVIDER`          | `AUTO`       | `AUTO`, `FIND_PACKAGE`, `SOURCE`, `FETCH`     |
| `BNIO_BEXEC_SOURCE_DIR`        | empty        | Path used by the `SOURCE` provider            |
| `BNIO_BEXEC_GIT_TAG`           | `main`       | Git ref used by the `FETCH` provider          |

When `bnio` is included with `add_subdirectory()` or `FetchContent`, tests,
examples, and installation rules default to off so they do not modify the
parent project's build.

### GoogleTest dependency

GoogleTest is resolved only after `BNIO_BUILD_TESTS` enables the `tests/`
subdirectory. With tests disabled, bnio does not call `find_package(GTest)`,
download GoogleTest, create GoogleTest targets, or expose it through the bnio
package.

The default `AUTO` provider accepts an existing `GTest::gtest_main` target,
tries `find_package(GTest)` (config and module modes), and then fetches the
pinned `BNIO_GOOGLETEST_GIT_TAG` as a fallback. The fetched copy does not build
GoogleMock and does not add GoogleTest installation rules.

```sh
# Require a preinstalled GoogleTest package; never download it.
cmake -S . -B build \
  -DBNIO_BUILD_TESTS=ON \
  -DBNIO_GOOGLETEST_PROVIDER=FIND_PACKAGE

# Skip all GoogleTest discovery and download work.
cmake -S . -B build-library -DBNIO_BUILD_TESTS=OFF
```

### bexec dependency providers

`bnio` always exposes the same `bnio::bnio` target, independently of how
`bexec` is supplied:

```sh
# Use an installed bexecConfig.cmake.
cmake -S . -B build \
  -DBNIO_BEXEC_PROVIDER=FIND_PACKAGE \
  -DCMAKE_PREFIX_PATH=/path/to/bexec

# Use a local bexec source checkout.
cmake -S . -B build \
  -DBNIO_BEXEC_PROVIDER=SOURCE \
  -DBNIO_BEXEC_SOURCE_DIR=/path/to/bexec

# Clone bexec during configuration.
cmake -S . -B build -DBNIO_BEXEC_PROVIDER=FETCH
```

`AUTO` first accepts an existing `bexec::bexec` target, then uses
`BNIO_BEXEC_SOURCE_DIR` when set, tries `find_package(bexec CONFIG)`, and
finally falls back to `FETCH`.

### Using a local bexec checkout

```sh
cmake -S . -B build \
  -DBNIO_BEXEC_PROVIDER=SOURCE \
  -DBNIO_BEXEC_SOURCE_DIR=/path/to/bexec
```

### Shared library build

```sh
cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON
cmake --build build-shared
```

Shared builds export the `BNIO_SHARED_LIBRARY` usage requirement, hide
non-public symbols, and install versioned library names with ABI version `0`.

### Use bnio as a dependency

All three consumption modes provide the `bnio::bnio` CMake target or equivalent
pkg-config link information.

#### Installed CMake package

Install `bexec` first, then build and install `bnio` against that package:

```sh
cmake -S . -B build-install \
  -DCMAKE_BUILD_TYPE=Release \
  -DBNIO_BEXEC_PROVIDER=FIND_PACKAGE \
  -DCMAKE_PREFIX_PATH=/install/prefix
cmake --build build-install
cmake --install build-install --prefix /install/prefix
```

A CMake consumer can then use:

```cmake
find_package(bnio 0.0.1 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE bnio::bnio)
```

#### pkg-config

The installation provides `bnio.pc`. On Linux it carries the required
`bexec`, OpenSSL, and liburing metadata:

```sh
c++ app.cpp $(pkg-config --cflags --libs bnio)
```

It can also be consumed as an imported CMake target:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(BNIO REQUIRED IMPORTED_TARGET bnio>=0.0.1)
target_link_libraries(your_target PRIVATE PkgConfig::BNIO)
```

#### Source tree

Direct source inclusion keeps the same namespaced target:

```cmake
add_subdirectory(path/to/bnio)
target_link_libraries(your_target PRIVATE bnio::bnio)
```

The parent may provide `bexec::bexec` first, select an installed package with
`BNIO_BEXEC_PROVIDER=FIND_PACKAGE`, point to a checkout with `SOURCE`, or allow
`bnio` to fetch it.

### Coverage report

Install `gcovr` 8.6 in a Python 3.10+ virtual environment, then configure an
instrumented build and run the tests:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install gcovr==8.6
cmake -S . -B build-coverage \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBNIO_BUILD_EXAMPLES=OFF \
  -DBNIO_ENABLE_COVERAGE=ON
cmake --build build-coverage
ctest --test-dir build-coverage --output-on-failure
cmake -E make_directory coverage
.venv/bin/gcovr --config gcovr.cfg build-coverage
```

On macOS, use AppleClang's gcov-compatible LLVM frontend for the final command:

```sh
.venv/bin/gcovr --config gcovr.cfg \
  --gcov-executable "xcrun llvm-cov gcov" build-coverage
```

The reports are written to `coverage/` as a text summary, detailed HTML, and
Cobertura XML. The GitHub Actions matrix uploads a separate artifact for the
Linux/io_uring and macOS/kqueue runs.

---

## License

MIT — see the [LICENSE](LICENSE) file for details.
