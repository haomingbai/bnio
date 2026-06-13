# Project Structure

This repository is a CMake-based C++ library for a small `liburing` wrapper.

- `include/bupp/bupp.h` is the top-level public umbrella header.
- `include/bupp/base.h` is the base-layer umbrella header.
- `include/bupp/base/linux/ring.h` wraps `io_uring`.
- `include/bupp/base/linux/params.h` wraps `io_uring_params`.
- `include/bupp/base/linux/submission_queue_entry.h` wraps `io_uring_sqe`.
- `include/bupp/base/linux/completion_queue_entry.h` wraps `io_uring_cqe`.
- `include/bupp/base/linux/probe.h` wraps `io_uring_probe`.
- `include/bupp/async_io/ip/` owns platform-neutral IP address and endpoint
  value types.
- `include/bupp/async_io/linux/` owns opt-in Linux native adapters.
- `include/bupp/linux/` owns the Linux implementation of the higher-level
  `bupp::io_context`.
- `include/bupp/buffer.h`, `include/bupp/tcp.h`, and `include/bupp/ssl.h` own
  the higher-level buffer, RAII TCP, and SSL stream APIs.
- `cmake/bexec.cmake` resolves the `bexec` sender/receiver dependency from a
  local source checkout or from GitHub.
- `src/async_io/` owns platform-neutral async I/O value implementations.
- `src/async_io/linux/` owns Linux-specific async I/O implementations.
- `src/base/linux/` owns base-layer method implementations.
- `src/linux/` owns Linux-specific higher-level context implementations.
- `src/` owns the compiled library target, built as static or shared through
  CMake's standard `BUILD_SHARED_LIBS` option.
- `tests/base/linux/` owns base-layer test executables and CTest registration.
- `tests/io_context/` owns higher-level `bupp::io_context` tests.
- `examples/base/linux/` owns base-layer example binaries.
- `examples/base/linux/README.md` describes the base examples as runnable reference
  programs.
- `examples/io_context/http_echo_server/` owns the `io_context` HTTP echo
  server example.
- `examples/benchmark/` owns optional benchmark-only targets enabled by
  `BUPP_BUILD_BENCHMARKS=ON`.
- `scripts/` contains project helper commands, including the optional
  `scripts/benchmark_http_echo.sh` benchmark runner.
