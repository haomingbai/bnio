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

`examples/mini_curl/` contains a compact HTTP/HTTPS client that demonstrates
**structured concurrency** with the sender/receiver pattern: the main thread
runs `io_context::run()`, and the operation's final receiver calls
`io_context::stop()` to shut down the context cleanly when the work is done.

### Usage

```sh
# HTTP GET
bnio_mini_curl http://httpbin.org/get

# HTTPS GET (auto-detected from https:// scheme)
bnio_mini_curl https://httpbin.org/get

# POST JSON data
bnio_mini_curl -X POST -H "Content-Type: application/json" \
  -d '{"key":"value"}' https://httpbin.org/post

# Download to file, follow redirects, with timeout
bnio_mini_curl -L -o output.html --timeout 30 https://example.com

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
|        | `--timeout SECONDS`  | Connection and transfer timeout                |
|        | `--host HOST`        | Override the URL host                          |
|        | `--port PORT`        | Override the URL port/service                  |
|        | `--path PATH`        | Override the request path                      |
|        | `--ipv4`             | Resolve only IPv4 addresses                    |
|        | `--ipv6`             | Resolve only IPv6 addresses                    |

### Design

The client uses structured concurrency with the sender/receiver pattern:

- **Structured concurrency** — main thread runs `io_context::run()`, and the
  operation's `final_receiver` calls `io_context::stop()` when the full
  request/response cycle (including redirects) completes. The context run loop
  exits cleanly once all work is drained.
- **Timeout support** — the `--timeout` option sets an overall deadline;
  if the operation does not complete within the given seconds, the timer fires
  and aborts the request.

Key source files under `examples/mini_curl/`:

- [`mini_curl.cpp`](../examples/mini_curl/mini_curl.cpp) — `main()`, argument
  parsing, URL parsing, and the event loop orchestration.
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
  completion receivers, including the `final_receiver` that stops the context.
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

## echo_server

`examples/echo_server/` — TCP echo server demonstrating structured concurrency.
The main thread runs `io_context::run()`. The `tcp_acceptor` accepts connections
via sender/receiver callbacks. Each connection runs read→write→read in an
`echo_session`. SIGINT triggers graceful shutdown: the acceptor closes, active
sessions drain, then `context.stop()` returns `run()`.

## dns_lookup

`examples/dns_lookup/` — minimal DNS resolution. Uses
`scheduler.async_resolve(host, service, dns_result_view)` with a receiver that
prints each resolved endpoint and calls `context.stop()`.

## tcp_client

`examples/tcp_client/` — TCP client with timeout. Full lifecycle:
resolve → connect → send → receive. A `steady_timer` watchdog (10 s)
aborts the request on timeout. Demonstrates sender/receiver chaining
through shared state; an `op_registry` keeps every operation state alive
until its completion is delivered.

## timer_chain

`examples/timer_chain/` — three timers in sequence (200ms → 100ms → 50ms).
Only the first timer is started before `run()`. Each receiver spawns the next
timer's sender, so the chain unfolds inside the event loop. The last receiver
calls `context.stop()`.

## timer_cancel

`examples/timer_cancel/` — timer cancellation on the normal path. A 100ms
timer cancels a pending 10s timer; the canceled wait completes with
`operation_canceled`, the receiver observes that error, and the context
stops for a clean, immediate exit.

## udp_echo

`examples/udp_echo/` — UDP send-and-receive. Opens a `udp_socket`, sends a
datagram with `async_send_to`, then waits for a reply with
`async_receive_from`. Completion stops the context. An `op_registry` keeps
the operation states alive until their completions are delivered.

## udp_connected

`examples/udp_connected/` — UDP connected mode. `udp::socket::connect()`
fixes the default peer, then `async_send` / `async_receive` exchange a
datagram without passing an endpoint each time — the counterpart to
`udp_echo`'s unconnected `*_to` / `*_from` forms.

## poll_fd

`examples/poll_fd/` — descriptor polling. Uses
`scheduler.async_poll(descriptor_view(STDIN_FILENO), POLLIN)` to wait for
stdin readability. The receiver reads a line and echoes it, then stops
the context.

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