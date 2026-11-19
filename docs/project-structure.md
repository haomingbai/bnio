# Project Structure

This repository is a CMake-based C++20 async I/O library.

> **Public API scope**: The public API consists only of the interfaces exposed
> through the umbrella header `<bnio/bnio.h>`. Subdirectories `async_io/` and
> `base/` are internal implementation details. Users should only
> `#include <bnio/bnio.h>`.

## Public Headers (`include/bnio/`)

### Umbrella headers

- `bnio.h` — top-level public umbrella header.
- `base.h` — base-layer umbrella header (Linux: `base/linux/`; BSD: `base/bsd/`).
- `async_io.h` — async-I/O-layer umbrella header.
- `io_context.h` — high-level `io_context` umbrella.
- `io_context_cpo.h` — CPO umbrella for generic scheduler/stream operations.
- `buffer.h` — buffer types umbrella.
- `tcp.h` — TCP socket/acceptor umbrella.
- `udp.h` — UDP socket umbrella plus `make_resolve_query()` helpers.
- `local.h` — local (AF_UNIX) socket/acceptor umbrella.
- `ssl.h` — SSL context/stream umbrella, including handshake-pause helpers.
- `ip.h` — IP address/endpoint convenience re-exports and DNS aliases.

### Internal Layer 1 — `bnio::base`

> **This is an internal implementation layer, not part of the public API.**
> Thin system call wrappers. Platform-specific headers under `base/linux/` and
> `base/bsd/`.

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

### Internal Layer 2 — `bnio::async_io`

> **This is an internal implementation layer, not part of the public API.**
> Platform-neutral vocabulary types and the platform-native event-loop context.

- `buffer_view.h` — non-owning byte range.
- `descriptor_view.h` — non-owning fd wrapper.
- `random_access_file.h` — non-owning positioned-I/O fd wrapper.
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
- `local/endpoint.h` — local (AF_UNIX) endpoint value type and address-form enum.
- `local/protocol.h` — local stream/datagram protocol tags with Asio-style
  discovery typedefs.
- `local/socket_view.h` — non-owning local stream/datagram socket views.
- `linux/io_uring_context.h` — umbrella for the Linux native context.
- `linux/io_uring_context_base.h` — umbrella for context, operation bases, and options.
- `linux/io_uring_context_base/context.h` — `io_uring_context` class definition.
- `linux/io_uring_context_base/operation_base.h` — intrusive CPU/I/O operation bases and shared queues.
- `linux/io_uring_context_base/options.h` — `io_uring_context_options`.
- `linux/io_uring_operations.h` — umbrella for concrete operation types.
- `linux/io_uring_operations/core.h` — nop, timeout, and internal operations.
- `linux/io_uring_operations/file.h` — aggregate for streaming and positioned
  file read/write operations.
- `linux/io_uring_operations/stream_file.h` — streaming descriptor
  read/write operations.
- `linux/io_uring_operations/random_access_file.h` — positioned random-access
  file read/write operations.
- `linux/io_uring_operations/poll.h` — descriptor poll operations.
- `linux/io_uring_operations/resolve.h` — DNS resolution operations.
- `linux/io_uring_operations/socket.h` — socket accept/connect/read/write operations.
- `linux/io_uring_operations/views.h` — view-based I/O operations.
- `linux/io_uring_operations/helpers.h` — operation traits and helpers.
- `linux/socket_address.h` — Linux-native socket address storage and endpoint conversion.
- `linux/detail/io_uring_receiver_operation.h` — receiver-based operation adapter.
- `bsd/kqueue_context.h` — umbrella for the BSD native context.
- `bsd/kqueue_context_base.h` — umbrella for context, operation bases, and options.
- `bsd/kqueue_context_base/context.h` — `kqueue_context` class definition.
- `bsd/kqueue_context_base/operation_base.h` — intrusive CPU/I/O operation bases and shared queues.
- `bsd/kqueue_context_base/options.h` — `kqueue_context_options`.
- `bsd/kqueue_helper.h` — readiness registration builder used by passive I/O operations.
- `bsd/kqueue_operations.h` — umbrella for concrete kqueue operation types.
- `bsd/kqueue_operations/core.h` — post, nop, and raw operations.
- `bsd/kqueue_operations/file.h` — aggregate for streaming and positioned
  file read/write operations.
- `bsd/kqueue_operations/file_common.h` — shared helpers for kqueue file
  read/write operations.
- `bsd/kqueue_operations/stream_file.h` — streaming descriptor read/write
  operations and low-level readiness operations.
- `bsd/kqueue_operations/random_access_file.h` — positioned random-access file
  read/write operations.
- `bsd/kqueue_operations/poll.h` — descriptor poll operations and sender.
- `bsd/kqueue_operations/resolve.h` — DNS resolution operations.
- `bsd/kqueue_operations/socket.h` — objectized nonblocking socket operations and typed senders.
- `bsd/socket_address.h` — BSD-native socket address storage and endpoint conversion.
- `bsd/detail/kqueue_receiver_operation.h` — receiver-based I/O operation adapter.

### Layer 3 — `bnio`

High-level async runtime, stream owners, and buffer types.

- `io_context.h` — public platform-neutral `io_context` umbrella.
- `detail/error_code.h` — shared empty `std::error_code` sentinel for async-I/O
  completion hot paths (internal implementation detail).
- `detail/posix/io_context/class.h` — the shared `io_context` class definition and
  scheduler surface.
- `detail/posix/io_context/native_context.h` — aliases the selected native context,
  options, operation bases, and task-queue state.
- `detail/posix/io_context/options.h` — `io_context_options` and the selected native
  context options.
- `detail/posix/io_context/timer_types.h` and `steady_timer.h` — timer operations,
  timer state, slot/heap types, and `steady_timer`.
- `detail/posix/io_context/native_io.h` — shared context/scheduler forwarding
  functions; selects backend request factories after the complete class
  definition.
- `detail/posix/io_context/timer_wait.h`, `read_all.h`, and `write_all.h` —
  shared timer wait and composed full-read/full-write sender templates; the
  read/write-all operation states are split by access kind into
  `stream_file_read_all.h`, `stream_file_write_all.h`, `random_access_read_all.h`,
  and `random_access_write_all.h`.
- `detail/posix/io_context/stream_file_io.h` and `random_access_file_io.h` —
  context/scheduler file I/O forwarding definitions, split by access kind.
- `detail/{linux,bsd}/io_context_native_io/` — backend-specific request
  adapters and factories (plus Linux SQE models); they remain explicit because
  readiness/retry and SQE/CQE lifecycles cannot be represented by an alias
  alone.
- `detail/posix/io_context/scheduler_operations.h` — scheduler-level I/O senders.
- `ssl/detail/common.h` — shared SSL operation helpers.
- `ssl/detail/buffers.h` — borrow-level buffer adapters for SSL operations.
- `ssl/detail/state_machine.h` — shared SSL operation state machine base.
- `ssl/detail/handshake_concepts.h` — handshake resume-factory concepts.
- `ssl/detail/handshake_pause_table.h` — mapping from OpenSSL pause returns
  to `handshake_pause_reason` values.
- `ssl/detail/handshake_retry.h` — factory-driven TLS handshake operation and
  sender (`handshake_retry_operation` / `handshake_retry_sender`); the
  2-argument handshake uses it with the default factory.
- `ssl/detail/shutdown.h` — TLS shutdown operation.
- `ssl/detail/senders.h` — SSL sender types; the `handshake_sender` alias
  instantiates the retry sender with the default factory, which reports
  callback-driven pauses as `std::errc::operation_not_supported`.
- `ssl/detail/read_write.h` — TLS read/write umbrella.
- `ssl/detail/read_write/state.h` — TLS read/write state.
- `ssl/detail/read_write/step.h` — TLS read/write state machine steps.
- `ssl/detail/read_write/operation.h` — TLS read/write operation.
- `ssl/detail/async_operations.h` — umbrella for SSL async operation
  implementations.
- `detail/tcp/async_operations.h` — TCP async operation sender factories.
- `detail/udp/async_operations.h` — UDP datagram sender factories.
- `detail/local/async_operations.h` — local (AF_UNIX) async operation sender
  factories.
- `buffer/` — buffer types:
  - `basic.h` — `mutable_buffer` and `const_buffer`.
  - `dynamic_string.h` — `dynamic_string_buffer`.
  - `dynamic_byte_vector.h` — `dynamic_byte_vector_buffer`.
  - `holders.h` — internal operation-lifetime buffer holders (non-owning with
    respect to caller storage).
- `tcp/` — TCP stream owners and layers:
  - `socket.h` — `tcp::socket` class (`tcp_socket` compatibility alias).
  - `acceptor.h` — `tcp::acceptor` class (`tcp_acceptor` compatibility alias).
  - `async_operations.h` — TCP async sender factories.
  - `layers.h` — TCP layer type list for SSL streams.
- `udp/` — UDP datagram owner and sender factories:
  - `socket.h` — `udp::socket` lifecycle and async sender declarations.
  - `async_operations.h` — connected and endpoint-aware UDP senders.
- `local/` — local (AF_UNIX) stream/datagram owners and sender factories:
  - `socket.h` — `local::stream_socket` and `local::datagram_socket` lifecycle
    and async sender declarations.
  - `acceptor.h` — `local::stream_acceptor` lifecycle and async sender
    declarations.
  - `async_operations.h` — stream read/write/connect/accept and datagram
    send/receive/send-to/receive-from senders.
- `ssl/` — TLS integration:
  - `base.h` — aggregate for the SSL base layer.
  - `base/errors.h` — OpenSSL error category and error_code helpers.
  - `base/context_base.h` — `ssl::base::context_base` class (RAII `SSL_CTX`
    owner; keeps the configuration/query surface and routes every method to
    the non-owning `view()`; holds no callback storage).
  - `base/context_view.h` — `ssl::base::context_view` non-owning view over a
    native `SSL_CTX` with the configuration/query surface.
  - `base/alpn.h` — destruction interface for heap-allocated callback
    arguments.
  - `base/bio_pair.h` — memory BIO pair assembly facility.
  - `callback.h` — callback result enumerators (`client_hello_result`,
    `cert_result`) and the framework-built proxies (`alert_out`, `sni_out`,
    `cert_out`) passed to user callbacks.
  - `context_view.h` — public `ssl::context_view` alias for the base view.
  - `handshake_pause.h` — public `handshake_pause_reason` enum, pause error
    category, and classification helpers (`is_handshake_pause`,
    `handshake_pause_reason_of`, `make_handshake_pause_error`).
  - `context.h` — `ssl::context` class (composes `base::context_base` with an
    inline `detail::callback_slot`; adds the method selector, forwarding
    configuration methods, the callback-invocability concepts, and the
    `set_alpn_callback`, `set_servername_callback`,
    `set_client_hello_callback`, and `set_cert_callback` closure installers).
  - `detail/callback_slot.h` — `detail::callback_base` destruction interface
    and the inline `detail::callback_slot` owning the context's closures.
  - `detail/owned_callback.h` — shared owned-closure helpers
    (`callback_closure_base`, `bound_invoker`, `make_closure`) and the concrete
    channel closures and trampolines (ALPN, SNI servername, client_hello,
    certificate). Every closure is reached directly through the native
    callback's `void*` argument.
  - `stream_view.h` — `ssl::stream_view` non-owning view over a native SSL
    connection.
  - `tcp/stream_class.h` — `ssl::tcp::stream<NextLayer>` class definition.
  - `tcp/stream_operations.h` — SSL stream async sender factories.
  - `stream.h` — aggregate for the ssl stream.
  - `cpo.h` — SSL CPOs (`async_handshake`, `async_shutdown`).
  - `detail/` — internal SSL async operation implementation (common helpers,
    buffer borrows, state machine, handshake/shutdown/read-write operations,
    sender types).
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
    `params.cpp`, `probe.cpp`, `wake_channel.cpp`.
- `src/base/bsd/` — base-layer BSD method implementations:
  - `kqueue.cpp`, `event.cpp`, `event_list_view.cpp`, `wake_channel.cpp`.
- `src/async_io/posix/` — POSIX-shared async_io implementations:
  - `address.cpp`, `tcp_endpoint.cpp`, `local_endpoint.cpp`.
- `src/async_io/linux/` — Linux-specific async_io implementations:
  - `address.cpp`, `dns.cpp`, `socket_address.cpp`, `socket_view.cpp` — Linux-native address, DNS, and socket adapters.
  - `io_uring_context.cpp` — lifecycle and queue init/exit.
  - `io_uring_context_cqe.cpp` — CQE dispatch.
  - `io_uring_context_run_loop.cpp` — main run loop.
  - `io_uring_context_io_tasks.cpp` — run-loop-only local/global I/O queue consumption and SQE preparation.
  - `io_uring_context_task_queue.cpp` — shared CPU/I/O publication, worker suspend-list registry, and wake channel I/O.
  - `io_uring_context_internal.h` — internal context state.
- `src/async_io/bsd/` — BSD-specific async_io implementations:
  - `address.cpp`, `dns.cpp`, `socket_address.cpp`, `socket_view.cpp` — BSD-native address, DNS, and socket adapters.
  - `kqueue_context.cpp` — lifecycle and queue init/exit.
  - `kqueue_context_events.cpp` — readiness collection and completion dispatch.
  - `kqueue_context_io_tasks.cpp` — run-loop-only passive I/O preparation and registration.
  - `kqueue_context_run_loop.cpp` — main run loop.
  - `kqueue_context_task_queue.cpp` — shared CPU/I/O publication, worker suspend-list registry, and per-worker/shared wake channels.
  - `kqueue_context_internal.h` — internal local task queue helpers.
  - `kqueue_helper.cpp` — readiness registration builder implementation.
- `src/posix/io_context.cpp`, `src/posix/io_context_queue.cpp`, `src/posix/io_context_timer.cpp`,
  and `src/posix/io_context_timer_state.cpp` — shared high-level `io_context`
  lifecycle, queue, timer, and timer-state implementations selected through
  `detail::native_context`.
- `src/posix/tcp.cpp` — TCP socket and acceptor methods.
- `src/posix/local.cpp` — local (AF_UNIX) socket and acceptor methods.
- `src/posix/ssl.cpp` — SSL base layer: OpenSSL error category,
  `ssl::base::context_base` ownership, and `ssl::base::context_view`
  operations.
- `src/posix/udp.cpp` — UDP socket methods.

## Tests (`tests/`)

The suite is organized by test weight; GoogleTest targets are registered under
the `basic`, `integration`, and `stress` CTest labels.

- `tests/basic/async_io/` — layer-2 unit tests: address, buffer_view,
  descriptor_view, dns_query, socket_view, tcp_endpoint, time,
  `local_endpoint`, Linux/BSD `socket_address` and `local_socket_address`,
  io_uring/kqueue operation traits, kqueue helper, and header
  self-containment.
- `tests/basic/base/` — Linux base-layer unit tests: SQE, CQE, params, probe,
  and header self-containment.
- `tests/basic/base_bsd/` — BSD base-layer header self-containment.
- `tests/basic/io_context/` — high-level unit tests: buffer, sender concepts,
  timer state, header self-containment, and the SSL callback/handshake-pause/
  handshake-retry unit and runtime tests.
- `tests/integration/async_io/` — io_uring and kqueue run-loop, sender, CQE
  dispatch, EAGAIN re-arm, error routing, SQ-full, UDP, and DNS tests.
- `tests/integration/base/` — Linux ring and wake-channel integration tests.
- `tests/integration/base_bsd/` — BSD kqueue and wake-channel integration
  tests.
- `tests/integration/io_context/` — `io_context` behavior: scheduler, sender
  concepts, accept/connect, read/write, read-all, poll, UDP, TCP, DNS, random
  access files, timers, local-socket stream/datagram/error/lifecycle tests,
  and the full SSL suite (handshake, transfer, ALPN, SNI, ClientHello,
  certificate callback, context switching, error precedence, error-queue
  hygiene, handshake-retry categories).
- `tests/integration/io_context/lifecycle/` — join, nested run, stop
  ordering/races, submit-after-stop, no-new-I/O-after-stop, and
  operation-leak/abort tests.
- `tests/stress/async_io/` — re-arm/stop stress.
- `tests/stress/base_bsd/` — wake-channel stress.
- `tests/stress/io_context/` — TCP, UDP, read/write, shutdown-submit, timer,
  timer-stop-race, local peer-close, SSL callback, and SSL handshake-retry
  stress.
- `tests/support/async_io/` — `io_uring_context_test_support.h` and
  `kqueue_context_test_support.h`.
- `tests/support/io_context/` — loopback, runtime, general context, and SSL
  test support headers.
- `tests/packaging/` — `consumer.cpp` plus `source/`, `cmake/`, and
  `pkgconfig/` consumer projects used by packaging tests.

## Examples (`examples/`)

All 11 example programs are defined as CMake targets:

- `examples/dns_lookup/` — DNS resolution (`bnio_dns_lookup`).
- `examples/echo_server/` — TCP echo server with graceful SIGINT shutdown
  (`bnio_echo_server`).
- `examples/file_cat/` — asynchronous `cat` over descriptor I/O
  (`bnio_file_cat`).
- `examples/mini_curl/` — full HTTP/HTTPS client with timeout support
  (`bnio_mini_curl`):
  - `mini_curl.cpp` — `main()` and argument parsing.
  - `request.cpp` — HTTP request construction.
  - `mini_curl_client.hpp` — aggregate header for the client components.
  - `mini_curl/` subdirectory — client, connection, receivers, redirect,
    transfer, output, operation_registry, and request headers.
- `examples/poll_fd/` — descriptor polling on stdin (`bnio_poll_fd`).
- `examples/ssl_dynamic_cert/` — per-SNI TLS certificate selection with a
  paused handshake (`bnio_ssl_dynamic_cert_server`,
  `bnio_ssl_dynamic_cert_client`).
- `examples/tcp_client/` — TCP client with timeout
  (`bnio_tcp_client`).
- `examples/timer_cancel/` — timer cancellation
  (`bnio_timer_cancel`).
- `examples/timer_chain/` — chained timers, 200ms→100ms→50ms
  (`bnio_timer_chain`).
- `examples/udp_connected/` — connected-mode UDP exchange
  (`bnio_udp_connected`).
- `examples/udp_echo/` — UDP send-and-receive (`bnio_udp_echo`).

## Benchmarks (`benchmarks/`)

- `benchmarks/throughput/` — functionally equivalent bnio and standalone-Asio
  TCP echo servers plus one shared client.
- `benchmarks/timer_churn/` — bnio and standalone-Asio active timer churn
  benchmarks with shared workload parsing and output formatting.

## Docs (`docs/`)

- `abi-compatibility.md` — ABI policy, dependency baseline, and release
  verification report.
- `maintaince.md` — naming, base-layer rules, test, and documentation
  conventions.
- `roadmap.md` — next milestone (QUIC).
- `design/architecture.md` — architecture overview and layer index.
- `design/architecture/layers.md` — layer overview and ownership model.
- `design/architecture/base-layer.md` — Layer 1 documentation.
- `design/architecture/async-io-layer.md` — Layer 2 documentation.
- `design/architecture/io-context-layer.md` — Layer 3 documentation.
- `design/architecture/header-namespace-map.md` — header dependency graph and
  namespace map.
- `design/architecture/io-context-read-all-eager.md` — read-all semantics and
  the eager-optional (immediate-I/O) design for `io_context`.
- `design/architecture/local-socket-split.md` — local (AF_UNIX) type-family
  design and implementation record.
- `design/architecture/worker-scheduling.md` — suspend list, directed wakeup,
  and local-queue scheduling.
- `design/lifecycle.md` — lifetime and ownership rules.
- `design/io_uring-setup.md` — io_uring setup flags and optimization.
- `design/timer.md` — timer subsystem design.
- `usage.md` → redirects to `usage/index.md`.
- `usage/index.md` — usage guide (sender/receiver, operations, local sockets,
  TLS, buffers, DNS, CPOs, submission modes, coroutines).
- `examples.md` — runnable example walkthrough.
- `build.md` — build, install, consumption, packaging (DEB/RPM), and coverage.
- `benchmark/index.md` — benchmark overview and entry point.
- `benchmark/io_uring.md` — io_uring (Linux) benchmark report.
- `benchmark/kqueue.md` — kqueue (macOS/BSD) benchmark report.
- `benchmark/charts/` — benchmark charts (io_uring/ and kqueue/ subdirectories).

## Build and Scripts

- `CMakeLists.txt` — top-level CMake project.
- `cmake/bexec.cmake` — resolves `bexec` from an installed CMake package, a
  local source tree, or FetchContent.
- `cmake/bnioConfig.cmake.in` — installed CMake package configuration.
- `cmake/bnio.pc.in` — relocatable pkg-config metadata template.
- `tests/packaging/` — source, installed-CMake, and pkg-config consumers.
- `scripts/` — project helper commands (format, check-doc, benchmark).
