# Examples

This page collects runnable examples for `bupp`. The examples are intentionally
small and show the lifetime rules around the event loop.

## Build Examples

Examples are enabled by default:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

To disable them:

```sh
cmake -S . -B build -DBUPP_BUILD_EXAMPLES=OFF
```

## Running an `io_context` Event Loop

`bupp::io_context` is a sender factory and an event loop. Calling an async
factory creates a sender. Connecting the sender creates an operation state.
Starting that operation queues or submits I/O. `ctx.run()` then waits for
completion events and delivers receiver callbacks.

```cpp
bupp::io_context ctx;
bupp::tcp_socket socket;
std::array<char, 4096> bytes{};

auto sender = ctx.async_receive(socket, bupp::buffer(bytes), 0);
auto op = bexec::connect(std::move(sender), my_receiver{});

bexec::start(op);
ctx.run();
```

The operation state must outlive the async operation. For a short program, a
stack variable is enough. For a server, keep operation states in an owning
object. The HTTP echo server example uses a small local holder for pending
accept, receive, send, and timer operations.

## `io_context` HTTP Echo Server

The standalone example is in
[`examples/io_context/http_echo_server`](../examples/io_context/http_echo_server).
It demonstrates:

- `tcp_acceptor` setup with `open`, `set_reuse_address`, `bind`, and `listen`
- repeated `ctx.async_accept(...)`
- per-connection `ctx.async_receive(...)` and `ctx.async_send(...)`
- `ctx.run()` as the server event loop
- explicit operation lifetime management for a long-running server

Build and run it:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target bupp_io_context_http_echo_server
./build/examples/io_context/http_echo_server/bupp_io_context_http_echo_server 8080
```

Try a request:

```sh
curl -v http://127.0.0.1:8080/hello
curl -v -d 'hello from bupp' http://127.0.0.1:8080/echo
```

Requests with a body receive the same body in the HTTP response. Requests
without a body receive a small text body containing the request method and
target.

## Base Linux Examples

The `examples/base/linux` directory demonstrates the low-level wrapper around
`liburing`. These examples use raw SQE/CQE handling and are useful when you want
to understand the layer under `io_context`.

## Optional Benchmark

Benchmark support is disabled by default. Enable the pieces you need:

| Option | What it does |
|---|---|
| `BUPP_BUILD_BENCHMARKS=ON` | Build the benchmark server executables |
| `BUPP_BUILD_ASIO_EXAMPLES=ON` | Auto-fetch standalone Asio (if not installed) |
| `BUPP_FETCH_WRK=ON` | Auto-fetch and build `wrk` from source |

All-in-one build:

```sh
cmake -S . -B build-benchmark -DCMAKE_BUILD_TYPE=Release \
  -DBUPP_BUILD_BENCHMARKS=ON \
  -DBUPP_BUILD_ASIO_EXAMPLES=ON \
  -DBUPP_FETCH_WRK=ON
cmake --build build-benchmark
```

Run the helper script:

```sh
scripts/benchmark_http_echo.sh
```

The script starts the bupp server and the Asio server one at a time, then runs
`wrk` against `http://127.0.0.1:<port>/echo`. It prefers the CMake-built wrk
at `${BUILD_DIR}/wrk-install/bin/wrk`, falling back to `PATH`.

Override defaults with environment variables:

```sh
THREADS=4 CONNECTIONS=128 DURATION=30s scripts/benchmark_http_echo.sh
```

### wrk auto-build notes

Building wrk from source requires:

- OpenSSL headers (already a bupp dependency)
- LuaJIT development headers: `luajit-devel` (Fedora) or `libluajit-dev` (Debian/Ubuntu)
- `make`

## Standalone Asio Echo Server

The `examples/asio_echo` directory contains an HTTP/1.1 echo server built on
**standalone Asio** (no Boost, no Beast). It is disabled by default. Enable it
with:

```sh
cmake -S . -B build-asio -DCMAKE_BUILD_TYPE=Debug -DBUPP_BUILD_ASIO_EXAMPLES=ON
cmake --build build-asio --target bupp_asio_echo_server
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

This mirrors `bupp::io_context`'s shutdown semantics (drain in-flight work
before stopping), while a raw `asio::io_context::stop()` exits immediately and
drops pending operations.

### Run

```sh
./build-asio/examples/asio_echo/bupp_asio_echo_server [port]
```

Default port is **8082** (8080 = bupp `io_context` server, 8081 = benchmark
Asio server).

```sh
curl -v http://127.0.0.1:8082/hello
curl -v -d 'hello from asio' http://127.0.0.1:8082/echo
```
