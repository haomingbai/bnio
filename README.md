# bupp

**bupp** is a modern C++20 async I/O library built on `liburing`. It provides a
scheduler-based async model — every I/O operation is a lazy sender that composes
with the standard receiver pattern. TCP, TLS (OpenSSL), DNS resolution, timers,
and composed writes all ship out of the box.

> **Status:** Linux-only (io_uring). OpenSSL ≥ 1.1 required for TLS support.

---

## Table of Contents

- [Dependencies](#dependencies)
- [Quick Start](#quick-start)
- [Examples](#examples)
  - [mini_curl](#mini_curl)
  - [Low-level base examples](#low-level-base-examples)
  - [Raw TCP echo](#raw-tcp-echo)
  - [Standalone Asio echo](#standalone-asio-echo)
- [Architecture](#architecture)
- [Build Options](#build-options)

---

## Dependencies

| Dependency    | Minimum Version | Notes                                |
| ------------- | --------------- | ------------------------------------ |
| C++ compiler  | C++20           | GCC 12+ or Clang 16+                 |
| CMake         | 3.20            |                                      |
| liburing      | 2.2             | `pkg-config` required                |
| OpenSSL       | 1.1             | `pkg-config` required; TLS feature   |
| bexec         | —               | Fetched automatically from GitHub    |

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
#include <bupp/bupp.h>

int main() {
  bupp::base::ring ring;
  if (ring.queue_init(8) < 0) {
    return 1;
  }

  bupp::base::submission_queue_entry sqe = ring.get_sqe();
  if (sqe.raw() == nullptr) {
    return 1;
  }

  sqe.prep_nop();
  sqe.set_data64(42);

  if (ring.submit() < 0) {
    return 1;
  }

  bupp::base::completion_queue_entry cqe;
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
bupp_mini_curl http://httpbin.org/get

# HTTPS GET (auto-detected from https:// scheme)
bupp_mini_curl https://httpbin.org/get

# POST JSON data
bupp_mini_curl -X POST -H "Content-Type: application/json" \
  -d '{"key":"value"}' https://httpbin.org/post

# Download to file, follow redirects
bupp_mini_curl -L -o output.html https://example.com

# HEAD request with verbose progress
bupp_mini_curl -I -v https://httpbin.org/status/200

# IPv4-only resolution, skip TLS verification
bupp_mini_curl -k --ipv4 https://localhost:8443/test
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

The client is split into two files:

- [`mini_curl.cpp`](examples/mini_curl/mini_curl.cpp) — `main()`, argument
  parsing, URL parsing, and HTTP protocol helpers.
- [`mini_curl_client.hpp`](examples/mini_curl/mini_curl_client.hpp) — the
  async client class with sender/receiver plumbing, DNS→connect→TLS→HTTP
  pipeline, redirect following, and SSL write-all loop.

Key patterns demonstrated:

- **Sender/receiver operation lifecycle** — every async step (resolve, connect,
  handshake, write, read, shutdown) is a sender connected to a receiver,
  managed by an `operation_registry`.
- **Endpoint fallback** — resolved endpoints are tried sequentially;
  connection failures advance to the next endpoint.
- **TLS handshake integration** — after TCP connect, the socket is moved into
  an `ssl_stream`; handshake, encrypted read/write, and shutdown all flow
  through the same scheduler API.
- **SSL partial-write loop** — `SSL_write` may produce short writes; the client
  retries until the entire request is sent.
- **Redirect following** — response headers are buffered until `\r\n\r\n`;
  3xx responses with a `Location` header trigger a new request (method changed
  to GET per RFC 7231).
- **Graceful TLS shutdown** — `SSL_ERROR_ZERO_RETURN` from the server is
  treated as a clean close; the client sends `close_notify` in response.

### Low-level base examples

`examples/base/linux/` contains small, direct programs that demonstrate the
`bupp::base` thin wrapper around `liburing`:

| Executable                    | What it does                                              |
| ----------------------------- | --------------------------------------------------------- |
| `bupp_base_probe`             | Print kernel opcode support                               |
| `bupp_base_nop`               | Smallest possible operation with `user_data` validation   |
| `bupp_base_timeout`           | Kernel timer completions                                  |
| `bupp_base_file_io`           | Write, fsync, read, close a temp file through io_uring    |
| `bupp_base_poll`              | Wait for pipe readiness                                   |
| `bupp_base_provided_buffers`  | Receive into kernel-selected provided buffers             |
| `bupp_base_echo_server`       | Echo server event loop (accept → recv → send)             |

```sh
# Run any example
./build/examples/base/linux/bupp_base_probe
./build/examples/base/linux/bupp_base_echo_server  # starts on port 7000
```

### Raw TCP echo

`examples/raw_echo/` contains a raw TCP echo server built on
`bupp::io_context`. It demonstrates the sender/receiver operation lifecycle and
uses `ctx.run()` as the event loop.

```sh
./build/examples/raw_echo/bupp_raw_echo [port]
# Default port: 8080
# Connect: printf 'hello' | nc 127.0.0.1 8080
```

### Standalone Asio echo

`examples/asio_echo/` contains an equivalent HTTP/1.1 echo server written
against **standalone Asio** (no Boost, no Beast). Disabled by default.

```sh
cmake -S . -B build-asio -DBUPP_BUILD_ASIO_EXAMPLES=ON
cmake --build build-asio --target bupp_asio_echo_server
./build-asio/examples/asio_echo/bupp_asio_echo_server [port]
```

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│  bupp::io_context  (event loop + scheduler)     │
│  bupp::ssl_stream  (TLS over any next layer)     │
│  bupp::tcp_socket / bupp::tcp_acceptor          │
├─────────────────────────────────────────────────┤
│  bupp::async_io  (non-owning views + DNS)        │
│  descriptor_view  buffer_view  dns_query         │
├─────────────────────────────────────────────────┤
│  bupp::base  (thin liburing wrapper)             │
│  ring  submission_queue_entry  completion_queue  │
└─────────────────────────────────────────────────┘
```text

- **`bupp::base`** — the thinnest possible wrapper around `liburing`. Owns the
  ring fd, exposes SQE preparation and CQE walking.
- **`bupp::async_io`** — platform-neutral vocabulary types. Non-owning views
  for descriptors, buffers, IP addresses/endpoints, and DNS queries. This layer
  intentionally has no senders, no RAII owners, no event loop.
- **`bupp::io_context`** — the high-level async runtime. Owns the event loop,
  produces schedulers (dispatch and post semantics), and provides sender
  factories for the lowest-layer socket views, file descriptors, DNS, polling,
  and timers. Stream owners build their higher-level senders on top.
- **`bupp::ssl_stream`** — an RAII TLS stream that layers over any next layer
  (default: `tcp_socket`). Owns the SSL object and memory BIOs. Senders for
  handshake, read, write, and shutdown are produced by the stream.

### Scheduler model

```cpp
bupp::io_context ctx;

// Post scheduler: schedule() always posts through the event loop.
auto sched = ctx.get_post_scheduler();

// Every async operation is a sender:
//   scheduler.async_resolve(query, result)
//   socket.async_connect(scheduler, endpoint)
//   scheduler.async_poll(descriptor, poll_mask)
//   scheduler.async_read(descriptor, buffer, offset)
//   scheduler.async_write(descriptor, buffer, offset)
//   ssl_stream.async_handshake(scheduler, type)
//   ssl_stream.async_write(scheduler, buffer)
//   ssl_stream.async_read(scheduler, buffer)
//   ssl_stream.async_shutdown(scheduler)

spawn(socket.async_connect(sched, endpoint), my_receiver{});
ctx.run();
```

---

## Build Options

| Option                    | Default  | Description                                 |
| ------------------------- | -------- | ------------------------------------------- |
| `BUILD_SHARED_LIBS`       | `OFF`    | Build `bupp` as a shared library            |
| `BUPP_BUILD_TESTS`        | `ON`     | Build and enable CTest                      |
| `BUPP_BUILD_EXAMPLES`     | `ON`     | Build example executables                   |
| `BUPP_BUILD_ASIO_EXAMPLES`| `OFF`    | Build Asio-based examples (fetches Asio)    |
| `BUPP_BEXEC_SOURCE_DIR`   | —        | Path to a local `bexec` checkout            |

### Using a local bexec checkout

```sh
cmake -S . -B build -DBUPP_BEXEC_SOURCE_DIR=/path/to/bexec
```

### Shared library build

```sh
cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON
cmake --build build-shared
```

---

## License

MIT — see the [LICENSE](LICENSE) file for details.
