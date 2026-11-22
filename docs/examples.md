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
`tcp_acceptor`, and `ssl::tcp::stream` expose the high-level async I/O sender
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
object. The raw echo server example uses a small operation registry for
pending accept, read, and write operations.

`async_read(...)` is read-all: it repeats bounded reads until the supplied
buffer is full or EOF arrives. `async_read_some(...)` is the one-attempt
spelling and returns one available chunk, which may be shorter than the
buffer. `async_write(...)` is write-all and sends the whole provided buffer
before completing; use `async_write_some(...)` when an example or application
needs to observe and retry short writes manually. TLS streams follow the same
naming: `async_read()` is a single `SSL_read` step, not read-all.

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
  an `ssl::tcp::stream`; handshake, encrypted read/write, and shutdown all flow
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
`echo_session`. The SIGINT/SIGTERM handler only saves and restores `errno` and
calls `context.request_stop()`; it does not touch the acceptor, session
bookkeeping, any `std::weak_ptr`, or atomic counters. The request wakes the
run loop, pending sessions are aborted through the cancellation channel, and
`run()` returns. The acceptor is then closed and the remaining cleanup runs
after `run()` returns, in ordinary code rather than inside the signal handler.

Every other example calls `io_context::stop()` from an ordinary completion
receiver. `stop()` is not async-signal-safe; a signal handler should call
`request_stop()` instead.

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

## file_cat

`examples/file_cat/` — asynchronous `cat` built as a repeat-until pipeline over
streaming descriptor I/O. The target is `bnio_file_cat`:

```sh
bnio_file_cat file.txt
```

The program opens the input file, then repeatedly reads up to 64 KiB with
`async_read_some` on an `async_io::descriptor_view` and writes the chunk to
`STDOUT_FILENO` with write-all `async_write`. A zero-byte read or an error ends
the loop. The round is composed with `bexec::let_value` (to carry the chunk
length and handle EOF/error uniformly) and `bexec::repeat_until`; the terminal
receiver stops the context. `SIGPIPE` is ignored so a closed stdout reader
(such as `bnio_file_cat big.log | head`) surfaces `EPIPE` through the completion
error code instead of terminating the process.

Expected behavior: the file contents appear on stdout, and the exit status is
`0` on success, `1` if the file cannot be opened or an I/O error occurs (with
the error and the byte count printed to stderr), and `2` for a usage error.

## ssl_dynamic_cert

`examples/ssl_dynamic_cert/` — per-SNI dynamic certificate selection with a
paused and resumed server handshake. The directory provides two targets,
`bnio_ssl_dynamic_cert_server` and `bnio_ssl_dynamic_cert_client`, and embeds a
throwaway test PKI in `certs.inc`: one CA and leaf identities for
`a.example.test` and `b.example.test`.

Server usage:

```sh
bnio_ssl_dynamic_cert_server [port] [mode]
```

- `port` defaults to `9443`.
- `mode` is optional and fixes one of four modes (`0`–`3`); when omitted, the
  server cycles through the modes per accepted connection.
- Bit 0 selects how the certificate is fetched during the paused handshake:
  `0` = direct factory, `1` = posted factory (the fetched result is delivered
  through the context rather than inline).
- Bit 1 selects how the certificate is installed: `0` = lazy per-SNI
  `ssl::context`, `1` = generic/shared `ssl::context`.

The server installs an SNI callback that pauses the handshake, looks up the
offered name, and resumes through `async_handshake(scheduler,
handshake_type::server, factory)`. It switches the connection to the selected
context with `stream.set_context()`, echoes one message, then stops. Unknown
SNI names are rejected. A 15-second watchdog aborts a run with no completed
connection.

Client usage:

```sh
bnio_ssl_dynamic_cert_client [port] [host]
```

- `port` defaults to `9443`; the client always connects to `127.0.0.1`.
- `host` defaults to `a.example.test` and is sent as the SNI name. The
  embedded CA verifies the server chain, and the client checks that the peer
  certificate covers `host`. Use `b.example.test` to select the other identity.

Both processes print progress. A successful client run shows the peer common
name, `verify=ok`, `host=ok`, the peer chain length, and finally `echo=ok`; the
server prints the accepted mode and a line such as
`handshake sni=a.example.test mode=0 fetch=direct install=lazy cn=a.example.test`.
A run with no completed exchange is aborted by the 10-second client watchdog
and the 15-second server watchdog.

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