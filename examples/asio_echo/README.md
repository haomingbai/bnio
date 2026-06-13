# Standalone Asio HTTP Echo Server

This optional subproject is disabled by default. Enable it to build a small
HTTP/1.1 echo server written against standalone Asio (no Boost, no Beast).

Asio is fetched automatically by CMake — no system package required.

## Build

```sh
cmake -S . -B build-asio -DCMAKE_BUILD_TYPE=Debug -DBUPP_BUILD_ASIO_EXAMPLES=ON
cmake --build build-asio --target bupp_asio_echo_server
```

If you already have standalone Asio installed system-wide, CMake will use that
copy instead of fetching.

## Run

```sh
./build-asio/examples/asio_echo/bupp_asio_echo_server [port]
```

Default port is **8082** (8080 = bupp `io_context` server, 8081 = benchmark
Asio server).

## Try it

```sh
# GET — body echoes method and target
curl -v http://127.0.0.1:8082/hello

# POST — body is echoed back
curl -v -d 'hello from asio' http://127.0.0.1:8082/echo

# HTTP/1.0 — Connection: close semantics
curl -v -H 'Content-Type: text/plain' -d 'http10 test' --http1.0 http://127.0.0.1:8082/test
```

## Graceful shutdown

Press Ctrl‑C (SIGINT) or send SIGTERM. The server:

1. Closes the acceptor — no more connections are accepted.
2. Closes every active session socket — pending `async_read_some` / `async_write`
   handlers complete with `operation_aborted`.
3. Each handler removes its session from the server's session set.
4. When the last session is gone, `ctx.stop()` is called and `run()` returns.

This mirrors `bupp::io_context`'s graceful shutdown semantics (drain in-flight
work before stopping the loop), unlike a naive `ctx.stop()` which exits
immediately and drops pending operations.

## Design notes

- HTTP parsing is hand-written (same logic shared with the benchmark and
  `io_context` examples). Keep-alive is supported via `Connection: keep-alive`.
- Sessions use `std::enable_shared_from_this` for safe async lifetime
  management.
- `asio::async_write` is used for responses — partial writes are handled
  automatically.
- The listener re-enters `async_accept` immediately so a slow connection never
  blocks new ones.
