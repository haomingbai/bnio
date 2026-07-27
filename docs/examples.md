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
