# Examples

This page collects runnable examples for `bnio`. The examples are intentionally
small and show the lifetime rules around the event loop.

## Build Examples

Examples are enabled by default:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

To disable them:

```sh
cmake -S . -B build -DBNIO_BUILD_EXAMPLES=OFF
```

## Running an `io_context` Event Loop

`bnio::io_context` owns the event loop. Streams such as `tcp_socket`,
`tcp_acceptor`, and `ssl_stream` expose the high-level async I/O sender
factories. The scheduler provides the low-level ability to operate on socket
views and file descriptors. Calling an async factory creates a sender.
Connecting the sender creates an operation state. Starting that operation
publishes I/O to the context queue. `ctx.run()` takes and submits that work,
then waits for completion events and
delivers receiver callbacks.

```cpp
bnio::io_context ctx;
auto scheduler = ctx.get_post_scheduler();
bnio::tcp_socket socket;
std::array<char, 4096> bytes{};

auto sender = socket.async_read(scheduler, bnio::buffer(bytes), 0);
auto op = bexec::connect(std::move(sender), my_receiver{});

bexec::start(op);
ctx.run();
```

The operation state must outlive the async operation. For a short program, a
stack variable is enough. For a server, keep operation states in an owning
object. The raw echo server example uses a small local holder for pending
accept, read, and write operations.

Read operations return one available chunk. Write operations send the whole
provided buffer before completing; use `async_write_some(...)` when an example
or application needs to observe and retry short writes manually.

## mini_curl

`examples/mini_curl/` contains a compact HTTP/HTTPS client that demonstrates the
full async stack: DNS resolution, TCP connect with fallback, TLS handshake,
HTTP request/response, and graceful shutdown — all via the scheduler API.

### Usage

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

### Options

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

### Design

The client is split into several files under `examples/mini_curl/`:

- [`mini_curl.cpp`](../examples/mini_curl/mini_curl.cpp) — `main()`, argument
  parsing, and URL parsing.
- [`request.cpp`](../examples/mini_curl/request.cpp) — HTTP request construction.
- [`mini_curl/request.hpp`](../examples/mini_curl/mini_curl/request.hpp) — request
  type definition.
- [`mini_curl/client.hpp`](../examples/mini_curl/mini_curl/client.hpp) — the
  async client class with sender/receiver plumbing, DNS→connect→TLS→HTTP
  pipeline, and redirect following.
- [`mini_curl/client_connection.hpp`](../examples/mini_curl/mini_curl/client_connection.hpp) —
  connection establishment with endpoint fallback.
- [`mini_curl/client_transfer.hpp`](../examples/mini_curl/mini_curl/client_transfer.hpp) —
  HTTP request/response transfer.
- [`mini_curl/client_receivers.hpp`](../examples/mini_curl/mini_curl/client_receivers.hpp) —
  completion receivers.
- [`mini_curl/client_redirect.hpp`](../examples/mini_curl/mini_curl/client_redirect.hpp) —
  redirect handling.
- [`mini_curl/client_output.hpp`](../examples/mini_curl/mini_curl/client_output.hpp) —
  response body output.
- [`mini_curl/operation_registry.hpp`](../examples/mini_curl/mini_curl/operation_registry.hpp) —
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

## Base Linux Examples

The `examples/base/linux` directory demonstrates the low-level wrapper around
`liburing`. These examples use raw SQE/CQE handling and are useful when you want
to understand the layer under `io_context`.

## TCP Throughput Benchmark

The throughput benchmark is in
[`benchmarks/throughput`](../benchmarks/throughput). It builds a bnio TCP echo
server, an Asio TCP echo server, and one shared Asio-based client. The same
client runs against both servers with the same connection count, duration, and
message size.

| Option | What it does |
|--------|-------------|
| `BNIO_BUILD_BENCHMARKS=ON` | Build throughput and timer benchmark targets |

Run the helper script:

```sh
scripts/benchmark.sh
```

Override defaults with environment variables:

```sh
CONNECTIONS=256 DURATION=30s MSG_SIZE=1024 scripts/benchmark.sh
```

Full benchmark reports with charts are in [benchmark/](benchmark/).

## Standalone Asio Echo Server

The `examples/asio_echo` directory contains an HTTP/1.1 echo server built on
**standalone Asio** (no Boost, no Beast). It is disabled by default. Enable it
with:

```sh
cmake -S . -B build-asio -DCMAKE_BUILD_TYPE=Debug -DBNIO_BUILD_ASIO_EXAMPLES=ON
cmake --build build-asio --target bnio_asio_echo_server
```

CMake auto-fetches Asio from GitHub. If you already have Asio installed
system-wide, the local copy is used instead.

### Graceful shutdown

This example implements **graceful shutdown** (unlike the benchmark server which
calls `ctx.stop()` directly):

1. SIGINT / SIGTERM → `server::request_shutdown()`
2. Acceptor is closed — no new connections
3. Every active session socket is closed — pending async handlers fire with
   `operation_aborted`
4. Each handler removes its session from the server's session set
5. When the last session is gone → `ctx.stop()` → `run()` returns

This mirrors `bnio::io_context`'s shutdown semantics (drain in-flight work
before stopping), while a raw `asio::io_context::stop()` exits immediately and
drops pending operations.

### Run

```sh
./build-asio/examples/asio_echo/bnio_asio_echo_server [port]
```

Default port is **8082** (8080 = bnio `io_context` server, 8081 = benchmark
Asio server).

```sh
curl -v http://127.0.0.1:8082/hello
curl -v -d 'hello from asio' http://127.0.0.1:8082/echo
```
