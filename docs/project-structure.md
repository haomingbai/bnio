# Project Structure

This repository is a CMake-based C++20 async I/O library.

## Public Headers (`include/bupp/`)

### Umbrella headers

- `bupp.h` — top-level public umbrella header.
- `base.h` — base-layer umbrella header (Linux: `base/linux/`; BSD: `base/bsd/`).
- `async_io.h` — async-I/O-layer umbrella header.
- `io_context.h` — high-level `io_context` umbrella.
- `io_context_cpo.h` — CPO umbrella for generic scheduler/stream operations.
- `buffer.h` — buffer types umbrella.
- `tcp.h` — TCP socket/acceptor umbrella.
- `ssl.h` — SSL context/stream umbrella.
- `ip.h` — IP address/endpoint convenience re-exports.

### Layer 1 — `bupp::base`

Thin system call wrappers. Platform-specific headers under `base/linux/` and
`base/bsd/`.

- `base/config.h` — base-layer compile-time knobs.
- `base/linux/ring.h` — RAII `io_uring` owner.
- `base/linux/submission_queue_entry.h` — non-owning `io_uring_sqe` view.
- `base/linux/completion_queue_entry.h` — non-owning `io_uring_cqe` view.
- `base/linux/params.h` — `io_uring_params` value type.
- `base/linux/probe.h` — RAII `io_uring_probe` owner.
- `base/linux/liburing.h` — liburing version/feature detection.
- `base/bsd/kqueue.h` — RAII `kqueue` fd owner.
- `base/bsd/event.h` — `struct kevent` wrapper.
- `base/bsd/event_list_view.h` — non-owning view over `kevent` arrays.

### Layer 2 — `bupp::async_io`

Platform-neutral vocabulary types and the platform-native event-loop context.

- `buffer_view.h` — non-owning byte range.
- `descriptor_view.h` — non-owning fd wrapper.
- `socket_view.h` — non-owning socket view family.
- `address.h` — platform-neutral IP address helpers.
- `tcp_endpoint.h` — TCP endpoint type.
- `dns.h` — DNS umbrella.
- `dns/query.h` — DNS query description.
- `dns/result.h` — DNS result storage.
- `dns/resolve.h` — DNS resolution sender.
- `dns/types.h` — DNS low-level types and flags.
- `time.h` — clock, duration, and time_point aliases.
- `config.h` — async_io compile-time knobs.
- `ip/address.h` — IPv4/IPv6 address value type.
- `ip/endpoint.h` — address + port value type.
- `ip/tcp.h` — TCP protocol tag.
- `ip/udp.h` — UDP protocol tag.
- `linux/io_uring_context.h` — umbrella for the Linux native context.
- `linux/io_uring_context_base.h` — umbrella for context + operation_base + options + submission.
- `linux/io_uring_context_base/context.h` — `io_uring_context` class definition.
- `linux/io_uring_context_base/operation_base.h` — intrusive `io_uring_operation_base`.
- `linux/io_uring_context_base/options.h` — `io_uring_context_options`.
- `linux/io_uring_context_base/submission.h` — template `prepare` / `submit` / `submit_batch`.
- `linux/io_uring_operations.h` — umbrella for concrete operation types.
- `linux/io_uring_operations/core.h` — nop, timeout, and internal operations.
- `linux/io_uring_operations/file.h` — regular-file read/write operations.
- `linux/io_uring_operations/poll.h` — descriptor poll operations.
- `linux/io_uring_operations/resolve.h` — DNS resolution operations.
- `linux/io_uring_operations/socket.h` — socket accept/connect/read/write operations.
- `linux/io_uring_operations/views.h` — view-based I/O operations.
- `linux/io_uring_operations/helpers.h` — operation traits and helpers.
- `linux/socket_address.h` — Linux socket address storage.
- `linux/detail/io_uring_receiver_operation.h` — receiver-based operation adapter.

### Layer 3 — `bupp`

High-level async runtime, stream owners, and buffer types.

- `io_context.h` → `linux/io_context.h` plus `io_context_cpo.h`.
- `linux/io_context.h` — `io_context` class definition and public scheduler
  surface; implementation detail types live under `linux/detail/`.
- `linux/detail/io_context_options.h` — `io_context_options` and `submit_mode`.
- `linux/detail/io_context_state.h` — grouped `io_context` native context,
  queued-I/O, and worker-list state.
- `linux/detail/io_context_state/native_worker.h` — per-run-thread native
  worker slot.
- `linux/detail/io_context_timer_types.h` — timer operations, timer state, slot,
  and heap types.
- `linux/detail/steady_timer.h` — `steady_timer` class definition.
- `linux/detail/io_context_native_io.h` — umbrella for native I/O operation templates.
- `linux/detail/io_context_native_io/common.h` — shared native-I/O helpers and
  the generic native operation/sender templates.
- `linux/detail/io_context_native_io/file.h` — descriptor read/write operation
  models.
- `linux/detail/io_context_native_io/poll.h` — poll operations.
- `linux/detail/io_context_native_io/socket.h` — stream and datagram socket
  accept/connect/read/write/send/receive operation models.
- `linux/detail/io_context_native_io/timer_wait.h` — timer wait operations.
- `linux/detail/io_context_native_io/write_all.h` — composed write-all sender.
- `detail/io_context/scheduler_operations.h` — scheduler-level I/O senders.
- `detail/ssl/async_operations.h` — umbrella for SSL async operations.
- `detail/ssl/async_operations/common.h` — shared SSL operation helpers.
- `detail/ssl/async_operations/handshake.h` — TLS handshake operation.
- `detail/ssl/async_operations/read_write.h` — TLS read/write umbrella.
- `detail/ssl/async_operations/read_write/operation.h` — TLS read/write operation.
- `detail/ssl/async_operations/read_write/state.h` — TLS read/write state.
- `detail/ssl/async_operations/read_write/step.h` — TLS read/write state machine.
- `detail/ssl/async_operations/senders.h` — SSL sender factories.
- `detail/ssl/async_operations/shutdown.h` — TLS shutdown operation.
- `detail/ssl/async_operations/state_machine.h` — SSL state machine helpers.
- `detail/tcp/async_operations.h` — TCP async operation sender factories.
- `detail/udp/async_operations.h` — UDP datagram sender factories.
- `buffer/` — buffer types:
  - `basic.h` — `mutable_buffer` and `const_buffer`.
  - `dynamic_string.h` — `dynamic_string_buffer`.
  - `dynamic_byte_vector.h` — `dynamic_byte_vector_buffer`.
  - `holders.h` — owning buffer wrappers.
- `tcp/` — TCP stream owners and layers:
  - `socket.h` — `tcp::socket` class (`tcp_socket` compatibility alias).
  - `acceptor.h` — `tcp::acceptor` class (`tcp_acceptor` compatibility alias).
  - `async_operations.h` — TCP async sender factories.
  - `layers.h` — TCP layer type list for `ssl_stream`.
- `udp/` — UDP datagram owner and sender factories:
  - `socket.h` — `udp::socket` lifecycle and async sender declarations.
  - `async_operations.h` — connected and endpoint-aware UDP senders.
- `ssl/` — TLS integration:
  - `context.h` — `ssl_context` class (RAII `SSL_CTX` owner).
  - `stream_class.h` — `ssl_stream<NextLayer>` class definition.
  - `stream.h` — `ssl_stream` template implementation.
  - `stream_operations.h` — SSL sender factories.
  - `cpo.h` — SSL CPOs (`async_handshake`, `async_shutdown`).
- `io_context_cpo/` — I/O CPOs and concepts:
  - `instances.h` — CPO object instances.
  - `concepts.h` — `reads_bytes`, `writes_bytes`, etc.
  - `read.h` — `async_read` / `async_read_some` CPOs.
  - `write.h` — `async_write` / `async_write_some` CPOs.
  - `connection.h` — `async_accept` / `async_connect` CPOs.
  - `poll.h` — `async_poll` CPO.
  - `resolve.h` — `async_resolve` CPO.
- `config.h` — top-level compile-time configuration.
- `config/system.h` — platform detection macros.
- `export.h` — symbol visibility macros.

## Source Files (`src/`)

- `src/CMakeLists.txt` — library target definition.
- `src/base/linux/` — base-layer Linux method implementations:
  - `ring.cpp`, `submission_queue_entry.cpp`, `completion_queue_entry.cpp`,
    `params.cpp`, `probe.cpp`.
- `src/base/bsd/` — base-layer BSD method implementations:
  - `kqueue.cpp`, `event.cpp`, `event_list_view.cpp`.
- `src/async_io/` — platform-neutral async_io implementations:
  - `address.cpp`, `socket_view.cpp`, `tcp_endpoint.cpp`.
- `src/async_io/linux/` — Linux-specific async_io implementations:
  - `address.cpp`, `dns.cpp`, `socket_address.cpp`.
  - `io_uring_context.cpp` — lifecycle and queue init/exit.
  - `io_uring_context_cqe.cpp` — CQE dispatch.
  - `io_uring_context_run_loop.cpp` — main run loop.
  - `io_uring_context_submission.cpp` — submission helpers.
  - `io_uring_context_task_queue.cpp` — task queue and non-atomic posted stack.
  - `io_uring_context_internal.h` — internal context state.
- `src/linux/` — high-level Linux `io_context` implementations:
  - `io_context.cpp` — lifecycle, schedulers, native worker slots.
  - `io_context_queue.cpp` — queued I/O submission.
  - `io_context_timer.cpp` — timer operation and slot management.
  - `io_context_timer_driver.cpp` — timer driver and heap.
  - `io_context_timer_state.cpp` — timeout/wakeup state machine.
- `src/tcp.cpp` — TCP socket and acceptor methods.
- `src/ssl.cpp` — SSL context and stream methods.

## Tests (`tests/`)

- `tests/base/linux/` — base-layer Linux tests (ring, SQE, CQE, params, probe,
  header self-containment).
- `tests/base/bsd/` — base-layer BSD tests (kqueue, header self-containment).
- `tests/async_io/` — async_io layer tests (buffer_view, descriptor_view,
  socket_view, address, DNS, tcp_endpoint, time, header self-containment).
  - `io_uring_context_test_support.h` — shared test support for io_uring_context.
  - `io_uring_cqe_dispatch_test.cpp` — CQE dispatch tests.
  - `io_uring_operation_traits_test.cpp` — operation trait tests.
  - `io_uring_run_loop_test.cpp` — run loop lifecycle tests.
  - `io_uring_sender_test.cpp` — sender-based operation tests.
- `tests/io_context/` — high-level `io_context` tests:
  - `io_context_test.cpp`, `io_context_scheduler_test.cpp`,
    `io_context_sender_concept_test.cpp`, `io_context_accept_connect_test.cpp`,
    `io_context_read_write_test.cpp`, `io_context_poll_test.cpp`.
  - `tcp_test.cpp`, `buffer_test.cpp`, `dns_test.cpp`.
  - `ssl_test.cpp`, `ssl_handshake_test.cpp`, `ssl_transfer_test.cpp`,
    `ssl_concept_test.cpp`.
  - `steady_timer_test.cpp`.
  - `header_self_contained_test.cpp`.
  - Various `*_test_support.h` files for shared test infrastructure.

## Examples (`examples/`)

- `examples/base/linux/` — low-level `liburing` wrapper examples (nop, probe,
  timeout, poll, echo_server).
- `examples/raw_echo/` — TCP echo server (`bupp_raw_echo.cpp`), Asio comparison
  (`asio_raw_echo.cpp`), and shared benchmark client (`client.cpp`).
- `examples/mini_curl/` — full HTTP/HTTPS client:
  - `mini_curl.cpp` — `main()` and argument parsing.
  - `request.cpp` — HTTP request construction.
  - `mini_curl/` subdirectory — client components (client, connection,
    receivers, redirect, transfer, output, operation_registry, request).
- `examples/asio_echo/` — standalone Asio HTTP echo server (disabled by default).

## Docs (`docs/`)

- `design/architecture.md` — architecture overview and layer index.
- `design/architecture/layers.md` — layer overview and ownership model.
- `design/architecture/base-layer.md` — Layer 1 documentation.
- `design/architecture/async-io-layer.md` — Layer 2 documentation.
- `design/architecture/io-context-layer.md` — Layer 3 documentation.
- `design/architecture/header-namespace-map.md` — header dependency graph and
  namespace map.
- `design/lifecycle.md` — lifetime and ownership rules.
- `design/io_uring-setup.md` — io_uring setup flags and optimization.
- `design/timer.md` — timer subsystem design.
- `design/kqueue-roadmap.md` — BSD kqueue portability roadmap.
- `usage.md` → redirects to `usage/index.md`.
- `usage/index.md` — usage guide (sender/receiver, operations, CPOs, submission
  modes, coroutines).
- `examples.md` — runnable example walkthrough.

## Build and Scripts

- `CMakeLists.txt` — top-level CMake project.
- `cmake/bexec.cmake` — resolves the `bexec` dependency.
- `scripts/` — project helper commands (format, check-doc, benchmark).
