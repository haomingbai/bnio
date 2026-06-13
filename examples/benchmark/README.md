# HTTP Echo Benchmark

This optional subproject is disabled by default. Enable it when you want to
compare the `bupp::io_context` HTTP echo server with a small standalone Asio
server that uses the same response logic.

Requirements:

- standalone Asio headers — auto-fetched when `BUPP_BUILD_ASIO_EXAMPLES=ON`;
  otherwise you need `libasio-dev` or equivalent
- `wrk` — auto-fetched and built when `BUPP_FETCH_WRK=ON`; otherwise install
  the `wrk` package from your distribution
- `luajit-devel` (Fedora) / `libluajit-dev` (Debian) — LuaJIT headers and
  static library required to build wrk from source

Build (all-in-one with auto-fetch):

```sh
cmake -S . -B build-benchmark -DCMAKE_BUILD_TYPE=Release \
  -DBUPP_BUILD_BENCHMARKS=ON \
  -DBUPP_BUILD_ASIO_EXAMPLES=ON \
  -DBUPP_FETCH_WRK=ON
cmake --build build-benchmark
```

Run the helper script from the repository root:

```sh
scripts/benchmark_http_echo.sh
```

The script starts one server at a time on loopback, runs `wrk`, and then stops
the server before moving to the next implementation.
