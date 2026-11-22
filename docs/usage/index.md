# Usage Guide

This guide introduces bnio in the order most users need it: first the
sender/receiver operation model, then the available operation families, then
the scheduler capability concepts used by generic code, followed by submission
mode choices and coroutine integration.

## Public API

All public types and functions are accessible through a single header:

```cpp
#include <bnio/bnio.h>
```

Only interfaces exposed through this header are part of the public API. All
public types live in the `bnio` namespace (or in sub-namespaces such as
`bnio::ip`, `bnio::tcp`, `bnio::udp`, `bnio::local`, and `bnio::ssl`). Internal
namespaces such as `bnio::async_io` and `bnio::base` are implementation details
and should not be used directly in application code. The vocabulary types they
expose in operation signatures — `bnio::async_io::buffer_view`,
`bnio::async_io::descriptor_view`, and `bnio::async_io::local::*` — remain
legal arguments because the public headers name them.

For architecture background, see [`../design/architecture.md`](../design/architecture.md).
For lifetime rules, see [`../design/lifecycle.md`](../design/lifecycle.md).
For runnable examples, see [`../examples.md`](../examples.md).

## 1. Basic sender/receiver operations

At the `io_context` layer, every asynchronous factory returns a lazy sender.
Creating the sender does not start I/O. The common lifecycle is:

```text
factory call -> sender -> bexec::connect(receiver) -> operation
operation -> bexec::start(operation) -> ctx.run() -> receiver completion
```

A minimal receiver handles the two completion channels bnio senders emit:

```cpp
struct read_receiver {
  // set_value carries the result. The leading std::error_code distinguishes
  // the outcomes that the operation can report itself:
  //   ec == {}       → success
  //   ec == canceled → cancelled by a non-token source: io_context::stop()
  //                    aborting the operation, a timer object-API
  //                    cancellation, or a kernel-level cancel
  //   ec == <other>  → recoverable I/O failure (errno-derived)
  void set_value(std::error_code ec, std::size_t n) noexcept {
    if (ec) {
      // Failure or non-token cancellation. Write-all/read-all senders
      // still report the bytes transferred so far through n.
    } else {
      // n bytes were transferred.
    }
  }

  // set_stopped is emitted if and only if the operation observed that the
  // stop token visible in its receiver environment was cancelled — the
  // cooperative, user-driven cancellation channel. It carries no payload.
  void set_stopped() noexcept {
    // The stop token was cancelled; no result is reported.
  }
};
```

A sender is connected to a receiver to create an operation state. Keep that
operation state alive until completion. In this small example the operation
lives on the stack until `ctx.run()` returns:

```cpp
#include <bnio/bnio.h>
#include <bexec/bexec.hpp>

#include <array>

int main() {
  bnio::io_context ctx;
  auto scheduler = ctx.get_post_scheduler();

  bnio::tcp_socket socket;
  // Assume socket has already been opened and connected.

  std::array<char, 4096> bytes{};
  auto sender = socket.async_read(scheduler, bnio::buffer(bytes), 0);
  auto operation = bexec::connect(std::move(sender), read_receiver{});

  bexec::start(operation);
  ctx.run();
}
```

`set_value(ec, ...)` is the universal result exit: success, recoverable
failure, and every non-token cancellation — `io_context::stop()` aborting
inflight I/O or not-yet-executed queued work, `steady_timer` object-API
cancellation, and kernel-level `ECANCELED` — all flow through it, with the
leading `std::error_code` distinguishing the case. `set_stopped()` is
reserved exclusively for cooperative cancellation: it is emitted if and only
if the operation observed, at `start()` or at the delivery point of queued
work, that the stop token visible in its receiver environment is cancelled —
including tokens forwarded or injected by composite sender algorithms
(`when_all`, `sync_wait`, repeat-style loops) in the standard cascading way.
`set_stopped()` carries no payload; operations cancelled through the stop
token report no byte counts. bnio's native operations are `noexcept`, and the
sender/receiver contract uses only these two completion channels:
`set_value(ec, ...)` and `set_stopped()`. `set_error` is not part of the
contract — no bnio `completion_signatures` include it, and no bnio receiver
implements it. Receivers may also expose `get_env()` when they need to
provide stop tokens or other receiver environment queries.

### Cancellation and stop

Which channel a cancellation fires on depends only on its source:

| Cancellation source | Completion |
|---------------------|------------|
| Stop token visible in the receiver environment (user-provided, or forwarded by a composite sender algorithm) | `set_stopped()` |
| `io_context::stop()` (or `request_stop()` whose Phase 2 is completed by a worker or `join()`) aborting inflight I/O or not-yet-executed queued work | `set_value(operation_canceled, ...)` |
| `steady_timer::cancel()`, `expires_after()`, timer destruction | `set_value(operation_canceled)` |
| `async_wait()` on an unregistered timer (moved-from `steady_timer`, or waits aborted by the context-stop Phase 2) | `set_value(operation_canceled)` |
| Kernel-level cancel (CQE / kevent reporting `ECANCELED`) | `set_value(ECANCELED, ...)` |

Rules that follow from the table:

- Both sources racing: when the stop token is cancelled and the context is
  stopping at the same time, the token wins — the operation completes with
  `set_stopped()`.
- `io_context::stop()` requests the stop and returns. Phase 1 only records
  the request; Phase 2 (abort pending timers, publish `stopping`) and the
  drain are completed by the workers as they leave `run()`, by `join()`, or —
  when no worker is running — synchronously by the `stop()` caller, which
  also drains the shared queues inline. `io_context::request_stop()` is the
  async-signal-safe Phase-1-only form: it never aborts timers, publishes no
  stopping state, and drains nothing, so after calling it from a signal
  handler still wait for `run()` to return (or use `join()`) before
  destroying the context or reusing buffers.
- An unregistered timer (`context == nullptr`, observable via
  `steady_timer::has_context()`) is legal to keep using: `cancel()` and
  `expires_after()` return `0` (no pending waits to cancel; `expires_at()`
  still updates the saved expiry), `expiry()` returns the saved expiry, and
  `async_wait()` completes inline with `set_value(operation_canceled)` — or
  `set_stopped()` when the receiver's stop token is already cancelled.
- Already-completed results are delivered unchanged: work whose result exists
  before a stop is not fabricated as cancelled.
- Queued I/O that has not reached the kernel is aborted by a stop and completes
  with `set_value(operation_canceled, ...)`. Operations that can complete
  eagerly (the probe inside `start()` finds the fd ready) are **best-effort**:
  the probe is a plain syscall outside the submission path, so a stop neither
  reverts it nor affects descriptor lifetime — its real result is delivered as
  it is. Checking stop state and keeping the operation's objects alive remain
  the receiver's responsibility. For TLS streams this covers
  staged completions too: a handshake or read/write result the SSL state
  machine has already produced (a TLS failure, an orderly close, transferred
  bytes) is delivered unchanged even when the context stopped before the
  final post-handoff to the receiver runs; the post step never overwrites the
  staged result with `operation_canceled`.
- Write-all and read-all senders report the bytes transferred so far when
  they complete with `set_value(operation_canceled, ...)`; on
  `set_stopped()` no byte count is reported.
- `io_context::run()` returns an empty error_code when it entered the run
  loop, including when that loop ended because of a stop. An entry rejected
  because a stop was already requested (`can_start_run()` is false) returns
  `operation_canceled` after draining whatever was already published. When
  the native backend fails to enter its run loop on a worker — the io_uring
  ring could not be enabled or a wake poll could not be armed — the result
  carries the backend's error instead: `std::error_code(-errno,
  std::generic_category())` from the failing enter step. Whatever was already
  published when the enter failed is still delivered to its receivers before
  `run()` returns.
- With `bexec::sync_wait`, token cancellation yields `std::nullopt`, while a
  context stop yields an engaged optional carrying `operation_canceled`.
- `when_all` over children aborted by a context stop aggregates their
  `set_value(operation_canceled, ...)` completions instead of
  short-circuiting with `set_stopped()`.
- DNS `async_resolve(...)` senders declare `set_stopped_t()` in their
  completion signatures: receivers connected to them must handle
  `set_stopped()` (source-breaking change; previously cancellation was
  reported through `set_value` only).

Schedulers are lightweight handles produced by `io_context`:

```cpp
bnio::io_context ctx;

auto post = ctx.get_post_scheduler();       // schedule() always posts.
auto dispatch = ctx.get_dispatch_scheduler(); // schedule() may run inline.
auto defer = ctx.get_defer_scheduler();     // schedule() and I/O always go to the shared queue.
```

The scheduler is passed to stream factories such as
`socket.async_read(scheduler, buffer)`.

The defer scheduler routes every submission it initiates — `schedule()` and
all I/O operations — through the shared queues, bypassing the worker-local
queues entirely, so any worker can take the work. Use it where pinning work
to the publishing worker's connection affinity is undesirable, such as an
accept loop whose re-arms should rotate across all workers.

## 2. Operation types

The high-level operation families are all ordinary senders. They differ only in
what work they start and what value they send on success.

| Operation family | Common APIs | Successful `set_value` |
|------------------|-------------|-------------------------|
| Scheduling | `bexec::schedule(scheduler)` | `()` |
| TCP accept | `tcp_acceptor::async_accept(...)` | `tcp_socket` |
| TCP connect | `tcp_socket::async_connect(...)` | `()` |
| UDP datagram | `udp::socket::async_send_to(...)`, `async_receive_from(...)` | `std::size_t` |
| Connected UDP datagram | `udp::socket::async_send(...)`, `async_receive(...)` | `std::size_t` |
| Local stream accept | `local::stream_acceptor::async_accept(...)` | `local::stream_socket` |
| Local stream connect | `local::stream_socket::async_connect(...)` | `()` |
| Local datagram | `local::datagram_socket::async_send_to(...)`, `async_receive_from(...)` | `std::size_t` |
| Connected local datagram | `local::datagram_socket::async_send(...)`, `async_receive(...)` | `std::size_t` |
| Byte reads | `async_read(...)`, `async_read_some(...)` | `std::size_t` |
| Byte writes | `async_write(...)`, `async_write_some(...)` | `std::size_t` |
| Descriptor polling | `async_poll(...)` | `unsigned` ready mask |
| DNS resolution | `async_resolve(...)` | `std::size_t` result count |
| Timer wait | `steady_timer::async_wait()` | `()` |
| TLS handshake/shutdown | `ssl::tcp::stream::async_handshake(...)`, `async_shutdown(...)` | `()` |
| TLS reads/writes | `ssl::tcp::stream::async_read(...)`, `async_write(...)` | `std::size_t` |

For `async_poll(...)`, the `unsigned` ready mask is meaningful only on success
(`ec == {}`). When the operation fails or is cancelled the mask is delivered as
`0`: the error itself is carried by the leading `ec`, never encoded into the
mask. A negative native result is never converted to `unsigned`, so no wrapped
value can reach a caller's `mask & POLLIN` test.

A descriptor-yielding operation never invents a descriptor.
`tcp_acceptor::async_accept(...)` sends a `tcp_socket` that owns a descriptor
only when the operation completes successfully; on failure or cancellation the
socket is closed and `native_handle()` is `-1`. `-1` is the only value that can
mean "no descriptor": `0` is a legal descriptor number, so unlike a byte count
a negative native result cannot be made harmless by clamping it with
`std::max(0, ...)`.

All of these senders complete with `set_value(std::error_code, ...)` as the
universal result exit (the leading `ec` distinguishes success, recoverable
failure, and non-token cancellation; see "Cancellation and stop" above).
`set_stopped()` is emitted if and only if the stop token visible in the
receiver environment is cancelled. The bnio sender/receiver contract uses
only these two completion channels; `set_error` is not part of it — no bnio
`completion_signatures` include it, and no bnio receiver implements it.

`ssl::tcp::stream::set_context(ssl::context&)` is the synchronous TLS control
primitive: it mirrors OpenSSL's `SSL_set_SSL_CTX` by replacing the context
behind an existing stream without rebuilding the SSL object or its BIOs. It
performs no certificate or SNI selection of its own, does not validate
certificates, and is intended to be called before the handshake. Pass the
owning `ssl::context` directly; a `context_view` overload remains for callers
that only hold a non-owning handle. The stream keeps no bnio reference to the
context. For dynamically looked-up callbacks (ALPN, SNI, client_hello), the
stream may outlive the owning context because destroying it clears those
registrations. A context with a certificate callback must outlive the stream
instead (see "Copied callbacks and lifetime" below). Moving the owning context
after installing a callback is safe because registrations point at heap
closure state. The call returns `std::error_code`, not a sender.

### TLS callbacks: ALPN, SNI, ClientHello, and certificate selection

`ssl::context` owns callback closures installed through
`set_alpn_callback()`, `set_servername_callback()`,
`set_client_hello_callback()`, and `set_cert_callback()`. All four use
`std::thread`-style argument binding: the callable and every bound argument
are decay-copied into a closure owned by the context, `std::reference_wrapper`
arguments are forwarded by reference, and reinstalling replaces and destroys
the previous closure. Every callable is invoked as

```cpp
result fn(context_view ctx, stream_view ssl, out_proxy& out, BoundArgs... args)
```

where both views are non-owning and valid only for the duration of the call.
Exceptions thrown by a callback never cross the OpenSSL C boundary; they fail
closed (ALPN and SNI send a fatal internal-error alert, ClientHello returns
`SSL_CLIENT_HELLO_ERROR` with an internal-error alert, certificate selection
aborts the handshake).

- **SNI** — `int fn(context_view, stream_view, sni_out&, ...)`. The returned
  `int` is passed through to OpenSSL (`SSL_TLSEXT_ERR_OK`,
  `SSL_TLSEXT_ERR_NOACK`, `SSL_TLSEXT_ERR_ALERT_FATAL`, ...).
  `sni_out::set_context(ctx)` mirrors `SSL_set_SSL_CTX` and switches the
  connection to another context; `sni_out::set_alert()` records the alert sent
  when the callback returns a fatal result. `ssl::stream_view::servername()`
  reports the offered name.
- **ClientHello** — `client_hello_result fn(context_view, stream_view,
  alert_out&, ...)`. `success`, `error`, and `retry` map directly to the
  matching `SSL_CLIENT_HELLO_*` values; `alert_out::set()` records the alert
  sent for `error`.
- **Certificate selection** — `cert_result fn(context_view, stream_view,
  cert_out&, ...)`. `cert_out::use_certificate()` and
  `cert_out::use_private_key()` mirror `SSL_use_certificate` and
  `SSL_use_PrivateKey`; OpenSSL increments the reference, so the caller keeps
  ownership. Both return `std::error_code`: success is an empty code, and a
  failed call reports the OpenSSL error that call recorded
  (`make_no_ssl_error()` is reserved for failures that never reached OpenSSL,
  such as a null connection). `fail` aborts the handshake; `ok` continues.

`context_view::use_certificate()` and `context_view::use_private_key()` install
an identity from memory on the native `SSL_CTX`, mirroring
`SSL_CTX_use_certificate` and `SSL_CTX_use_PrivateKey`.

**Context switching.** `sni_out::set_context()` inside an SNI callback and
`ssl::tcp::stream::set_context()` before the handshake both mirror
`SSL_set_SSL_CTX`. The SSL object takes its own reference to the native
`SSL_CTX`, so for dynamically looked-up callbacks (ALPN, SNI, client_hello)
the bnio owner may be destroyed afterwards: its registrations are cleared,
and none of those callbacks runs for the connection. Certificate selection is
different: `SSL_set_SSL_CTX` duplicates the target context's `cert_cb` and
`cert_cb_arg` into the connection, so a target context with a certificate
callback must outlive the stream. Switching to a context that is destroyed
earlier is unsupported.

**Callback-driven retry.** `client_hello_result::retry` and
`cert_result::retry` pause the handshake. Resume them with the 3-argument
`async_handshake(scheduler, type, factory)` overload: the factory receives
only callback-driven pause codes
(`handshake_pause_reason::client_hello_cb`,
`handshake_pause_reason::x509_lookup`, `handshake_pause_reason::async`,
`handshake_pause_reason::async_job`, and
`handshake_pause_reason::retry_verify`) and returns a sender that performs
the user's asynchronous work; an empty completion resumes the handshake.
Transport progress (`handshake_pause_reason::want_read` and
`handshake_pause_reason::want_write`) is internal to the operation and never
reaches the factory. The 2-argument overload has no resume surface, so a
callback-driven pause completes with `std::errc::operation_not_supported`.

Pause codes use a dedicated public error category, declared in
`<bnio/ssl/handshake_pause.h>` and included by `<bnio/bnio.h>`:
`handshake_pause_category()`, `make_handshake_pause_error(reason)`,
`is_handshake_pause(ec)`, and `handshake_pause_reason_of(ec)`. The factory
receives a plain `std::error_code`; use `handshake_pause_reason_of()` rather
than casting the stored value. `handshake_pause_reason::none` yields an empty
code and is also returned for unknown category values.

**Copied callbacks and lifetime.** Callbacks split into two lifetime rules.
ALPN, SNI servername, and ClientHello registrations are looked up through the
connection's current `SSL_CTX` at invocation time, so a stream may outlive the
owning context; destroying the context disables those callbacks. The
certificate callback is copied by OpenSSL into each SSL at `SSL_new` and
`SSL_set_SSL_CTX` time, so the owning `ssl::context` must outlive every stream
that may invoke it, and the callback must not be replaced or cleared while
such a stream is alive. The same rule will apply to future copied-argument
callbacks. If you need a stream to outlive a context with a certificate
callback, use the raw native setter through `context_view::native_handle()`
and manage the callback and its argument lifetime yourself; the owning API
does not support that pattern.

**Lifetime.** A context with a certificate callback must outlive every stream
that may invoke it. For ALPN, SNI, and ClientHello the owning context only
needs to outlive the streams whose handshake should still invoke those
callbacks; destroying it disables them for connections that outlive it
without leaving them with dangling state. Moving a context is safe in either
case. See Rule 6 in [`design/lifecycle.md`](../design/lifecycle.md) for the
per-channel details.

TLS streams encode EOF and close results the same way as plain descriptors
and TCP sockets:

- `ssl::tcp::stream::async_read()` and `ssl::tcp::stream::async_read_some()` both mean
  one plaintext `SSL_read` attempt. There is no read-all form for TLS: TLS
  record boundaries do not map to a caller's buffer size, so a call reports
  the bytes of a single `SSL_read` step.
- An orderly TLS close (the peer sent `close_notify`) completes a read
  successfully with `set_value({}, 0)` — the same EOF encoding as the
  zero-byte read on a plain descriptor or TCP socket.
- A transport-level close without `close_notify` is a truncated stream: the
  read completes with `set_value(connection_reset, n)`, the same pass-through
  a TCP socket reports on `ECONNRESET`.
- `ssl::tcp::stream::async_write()` is write-all. If the peer disappears mid-write
  (a transport write of 0 bytes, or `SSL_ERROR_ZERO_RETURN` during a write
  step), the write completes with `set_value(broken_pipe, n)`, matching the
  write-all zero-byte encoding for TCP.

TLS failure error codes are attributed to the failing OpenSSL call. bnio
clears the calling thread's OpenSSL error queue immediately before every
OpenSSL call whose failure is reported through the queue, so the `ec` a
failed handshake, read, write, or shutdown completes with is the error that
call recorded — never a leftover from earlier OpenSSL work on the same
thread. When a failure path reports no OpenSSL error at all (for example, a
handshake on an invalid stream never reaches OpenSSL), the operation
completes with `bnio::ssl::make_no_ssl_error()`: a dedicated value in the OpenSSL
error category whose message is "no OpenSSL error was recorded". It never
collides with a real OpenSSL error code and does not represent any TLS-level
failure.

Read and write names are intentionally precise:

`async_read()` is read-all for TCP streams and file descriptors. It repeats
bounded native reads until the supplied buffer is full or EOF is observed. EOF
(a native read returning 0) completes successfully with `ec={}` and the number
of bytes read so far; a size of `0` means EOF arrived before any data. A
first-step error propagates its `ec` directly.

`async_read_some()` is the explicit spelling for one read attempt. It completes
with the byte count of one bounded native read, which may be fewer than the
buffer size, including `0` for EOF on plain descriptors or TCP sockets. Use it
when the caller wants manual framing or single-read behavior.

`async_write()` is write-all for TCP streams, TLS streams, and file
descriptors. It repeats bounded native writes until the entire buffer is
transferred, or until an error, a non-token cancellation, or a stop-token
cancellation occurs.

`async_write_some()` performs one bounded write attempt and reports that
attempt's byte count. Use it when the caller wants manual framing, retry, or
backpressure policy.

File descriptors come in two families. On `async_io::descriptor_view` the four
operations are **streaming**: they transfer at the kernel file position and
advance it, exactly like `::read()`/`::write()`, so sequential pipelines track
no offsets of their own. On `async_io::random_access_file` (a non-owning view
constructed from an fd or a `descriptor_view`) the same four operations are
**positioned**: each takes a mandatory trailing `offset` argument and leaves
the kernel file position untouched; read-all/write-all on this view advance
through `offset + transferred` per step, and an offset beyond the `int64_t`
range fails with `EOVERFLOW`. Use streaming for sequential files, pipes, and
ttys; use positioned I/O when the caller computes every offset (parallel range
I/O, headers, database pages). Keep at most one streaming operation in flight
per descriptor — concurrent streaming operations race on the shared kernel
position — while positioned operations on one file may overlap freely. A
streaming write on an `O_APPEND` descriptor is the kernel-atomic append.

UDP operations always transfer exactly one datagram. `async_send_to()` and
`async_receive_from()` preserve message boundaries and never use stream-style
write-all retries. Call `udp::socket::connect(endpoint)` to set a default peer,
then use `async_send()` and `async_receive()`. The endpoint passed to
`async_receive_from()` is output storage and must remain alive until the sender
completes.

Use `udp::make_resolve_query(host, service, protocol)` to create a
`dns_query` with the UDP transport and address-family filters already set, then
pass it to the existing `async_resolve()` API.

### Local (AF_UNIX) sockets

`bnio::local` is a separate type family for AF_UNIX sockets, mirroring the TCP
and UDP surface. All local owners are move-only RAII types whose `open()`,
`bind()`, `connect()`, `listen()`, `close()`, and `shutdown()` calls return
`std::error_code` rather than throwing.

Endpoints are `bnio::async_io::local::endpoint` values:

- `endpoint()` is `endpoint_kind::unspecified`; it is also the form decoded
  from an unnamed peer.
- `endpoint(path)` is a filesystem path (`endpoint_kind::path_name`).
  `path()` returns the stored path without a leading NUL and without a
  terminator byte.
- `endpoint::abstract(name)` creates an abstract-namespace endpoint and exists
  only on Linux (`BNIO_HAS_ASYNC_IO_LINUX`); the leading NUL is implicit.
- `max_path_length` is `sizeof(sockaddr_un::sun_path)` — 108 on Linux, 104 on
  BSD. An oversized path leaves the endpoint unspecified, and the consuming
  socket call reports `std::errc::invalid_argument` without issuing a syscall.

`local::stream_protocol` and `local::datagram_protocol` are the protocol tags
passed to `open()`. Stream sockets and acceptors use
`async_io::local::stream_socket_view`; datagram sockets use
`async_io::local::datagram_socket_view`.

```cpp
bnio::io_context ctx;
auto scheduler = ctx.get_post_scheduler();

const bnio::async_io::local::endpoint path("/tmp/bnio-demo.sock");

bnio::local::stream_acceptor acceptor;
// Error codes omitted here for brevity; every call returns std::error_code.
acceptor.open(bnio::async_io::local::stream_protocol{});
acceptor.bind(path);
acceptor.listen(16);

// Completes with set_value(std::error_code, bnio::local::stream_socket).
auto accept_sender = acceptor.async_accept(scheduler);

bnio::local::stream_socket client;
client.open(bnio::async_io::local::stream_protocol{});
auto connect_sender = client.async_connect(scheduler, path);
```

`local::stream_socket` provides `async_read` / `async_read_some` /
`async_write` / `async_write_some` with the same read-all, write-all, and
one-attempt semantics as TCP. `local::stream_acceptor::async_accept()` owns the
accepted descriptor and completes with a `local::stream_socket`; as with TCP,
the socket is closed and `native_handle()` is `-1` when the operation fails or
is cancelled.

`local::datagram_socket` preserves message boundaries: `connect(endpoint)` sets
the default peer for `async_send()` / `async_receive()`, and
`async_send_to()` / `async_receive_from()` address each datagram explicitly.
The endpoint passed to `async_receive_from()` is output storage and must stay
alive until the sender completes.

Binding over an existing socket file fails with `EADDRINUSE`; unlinking a stale
path is the caller's job. Other common errors are `ECONNREFUSED` (connect to a
bound-but-not-listening or stale path), `EPIPE` (peer closed on send),
`ENOTCONN` (send without a default peer), `EAFNOSUPPORT` (wrong address family),
and `invalid_argument` (unspecified or oversized endpoint). Endpoint decode
failures on receive are reported as `address_family_not_supported` with the
endpoint reset, matching the IP decoders. `SOCK_SEQPACKET`, `connect_pair`,
credential passing, and descriptor passing are not part of this API.

### TLS configuration

`bnio::ssl::context` owns an `SSL_CTX`. The constructor takes a
`bnio::ssl::context_method`:

- `context_method::tls` — generic `TLS_method()` (the default);
- `context_method::tls_client` — `TLS_client_method()`;
- `context_method::tls_server` — `TLS_server_method()`.

Load the server identity with `use_certificate_chain_file()` and
`use_private_key_file()` (or the in-memory `use_certificate()` /
`use_private_key()` pair), then verify it with `check_private_key()`. Peer
verification is configured with `set_verify_mode()` using the usual OpenSSL
`SSL_VERIFY_*` flags; `set_verify_depth()` bounds the chain.
`load_verify_locations(ca_file, ca_dir)` loads explicit trust anchors, and
`set_default_verify_paths()` loads the system default locations.

ALPN has two directions. A client installs its wire-format preference list
with `set_alpn_protos(data, length)`, where each protocol name is prefixed by a
one-byte length. A server installs an `int`-returning
`set_alpn_callback(fn, args...)` closure whose signature receives
`(context_view, stream_view, context::alpn_out&, std::span<const unsigned char>)`;
call `alpn_out::set(data, len)` to select a protocol or leave it untouched to
skip ALPN. `ssl::tcp::stream::get_alpn_selected()` and
`ssl::stream_view::alpn_selected()` report the negotiated protocol as an empty
span when none was selected.

A TLS stream wraps a connected lower layer:
`bnio::ssl::tcp::stream<bnio::tcp_socket> stream(std::move(socket), ctx);`.
Start the handshake with `async_handshake(scheduler, handshake_type::client)`
or `handshake_type::server`; the two-argument overload completes with
`set_value(std::error_code)`. The three-argument overload adds a resume factory
for callback-driven pauses (see below). `async_shutdown()` sends
`close_notify`. `set_context(ssl::context&)` mirrors `SSL_set_SSL_CTX` and is
the primitive used for SNI-driven context switching before or during the
handshake.

```cpp
bnio::io_context ctx;
auto scheduler = ctx.get_post_scheduler();

// Error codes omitted here for brevity; every call returns std::error_code.
bnio::ssl::context server_ctx(bnio::ssl::context_method::tls_server);
server_ctx.use_certificate_chain_file("cert.pem");
server_ctx.use_private_key_file("key.pem");
server_ctx.check_private_key();
server_ctx.set_verify_mode(SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
server_ctx.set_default_verify_paths();

bnio::ssl::context client_ctx(bnio::ssl::context_method::tls_client);
client_ctx.set_default_verify_paths();
const unsigned char alpn[] = {2, 'h', '2'};
client_ctx.set_alpn_protos(alpn, sizeof alpn);

bnio::tcp_socket socket;
bnio::ssl::tcp::stream<bnio::tcp_socket> stream(std::move(socket), client_ctx);
auto handshake =
    stream.async_handshake(scheduler, bnio::ssl::handshake_type::client);
auto negotiated = stream.get_alpn_selected();
```

[`examples/ssl_dynamic_cert`](../../examples/ssl_dynamic_cert) combines
per-SNI certificate selection, a paused and resumed handshake, and embedded
test identities; see [`../examples.md`](../examples.md#ssl_dynamic_cert).

The sender model is the same across all operation levels. For example,
`tcp_acceptor::async_accept(scheduler, flags)` completes with a `tcp_socket`,
which owns the underlying file descriptor so users do not need to manage raw
native handles.

## 3. Buffers, addresses, and context control

### Buffers and dynamic buffers

`bnio::mutable_buffer` and `bnio::const_buffer` are non-owning
`(pointer, size)` views. The free function `bnio::buffer(...)` builds them
from a pointer plus size, `std::span`, `std::array`, `std::vector`,
`std::string`, `std::string_view`, or an `bnio::async_io::buffer_view`.
`async_io::buffer_view` is the layer-2 aggregate `{void* data; std::size_t
size;}` and is accepted anywhere a buffer is expected.

Every async operation takes an internal holder that keeps the *view* or
*dynamic-buffer adapter* alive for the operation's lifetime. It does not own
the underlying bytes:

- For `mutable_buffer`, `const_buffer`, and `buffer_view`, the caller owns the
  storage and must keep it valid and unmoved until the completion runs.
- `bnio::dynamic_string_buffer` and `bnio::dynamic_byte_vector_buffer<A>`
  (created with `bnio::dynamic_buffer(storage)`) reference the caller's
  `std::string` or `std::vector<std::byte>`; the adapter does not own that
  storage either. A read operation calls `prepare()` on the adapter and
  `commit(n)` after a successful step, so the underlying container grows in
  place and must not be destroyed or replaced while the operation is pending.
  `consume(n)` is available for caller-side framing.

`async_read` / `async_write` are read-all / write-all; use
`async_read_some` / `async_write_some` for a single bounded native attempt.
The dynamic-buffer `commit()` only runs for an operation that can consume a
buffer (reads and receive-from); write paths take an immutable view.

### Closing sockets, descriptors, and in-flight operations

`close()` closes the owned descriptor and returns it to the OS. It does not
cancel operations that are already in flight. On io_uring, an already-submitted
request holds its own reference to the file, so it still completes through its
CQE after `close()`; the receiver may run after the descriptor is gone, and the
result is whatever the kernel reports. Keep the operation state and every
buffer it references alive until that completion runs.

If the context is stopped while the operation is still in flight, the
cancellation path takes over. Keep the buffer until the `run()` call on the
thread that owns the operation returns, or until `join()` has completed
(which waits for every `run()` to return and for the shared-queue drain), not
merely until the receiver's `set_value(operation_canceled)` runs. `stop()`
alone is not a synchronization point for buffer reuse, and `request_stop()`
is weaker still — it only records the request. See "Cancellation and stop"
above.

On kqueue, closing a descriptor removes the kevents registered for it. An
operation that was waiting on that descriptor may therefore never complete on
its own. Do not call `close()` while operations are in flight on kqueue: wait
for them, or stop the context and wait with `join()`.

`release()` transfers descriptor ownership to the caller and does not cancel
in-flight operations. `assign()` adopts a descriptor; when the object already
owns one, it closes the old descriptor first. Neither cancels outstanding
work. `shutdown(how)` only disables one or both directions of a connected
stream socket; it neither closes the descriptor nor cancels operations.

TLS is two-layered: `ssl::tcp::stream::async_shutdown()` sends the TLS
close_notify and leaves the transport open; close the transport afterwards
with `lowest_layer().close()`. If the connection must be aborted instead, stop
the context (or close the transport and let the resulting completions run).

There is no per-object `cancel()` in this release. The only operation-level
cancel is `steady_timer::cancel()`; context-wide cancellation is
`io_context::stop()` (or `request_stop()` followed by `join()`).

### IP addresses and DNS result types

`bnio::ip::address` and `bnio::ip::endpoint` are the public IP value types.
Endpoints provide `loopback_v4(port)`, `any_v4(port)`, `loopback_v6(port)`,
and `any_v6(port)`, plus `address()` and `port()` accessors. When
`BNIO_HAS_ASYNC_IO_IP_ADDRESS_PARSER` is defined (the default on both POSIX
backends), `bnio::ip` re-exports the parsers:

```cpp
auto a = bnio::ip::make_address("127.0.0.1");   // optional<ip::address>
auto b = bnio::ip::make_addr("::1");            // alias of make_address
auto c = bnio::ip::make_v4_address("10.0.0.1"); // IPv4 only
auto d = bnio::ip::make_v6_address("fe80::1");  // IPv6 only
```

Each returns `std::optional<address>` and yields `std::nullopt` for malformed
input.

The resolver API uses three public types:

- `bnio::dns_query` — fixed-capacity owned query (`host`, service name or
  decimal port string, address-version filter, transport filter, flags). An
  over-capacity input makes `valid()` false and resolution reports
  `std::errc::invalid_argument`.
- `bnio::dns_result_view` — non-owning view over caller-owned
  `bnio::ip::endpoint` storage. The view reports capacity through `size()`;
  the number of endpoints actually written is reported separately by the
  operation's success value (`std::size_t` count), not by the view. Iteration
  over the view covers the full capacity.
- `bnio::dns_transport` / `bnio::dns_query_flags` — filters
  (`any`, `tcp`, `udp`) and flags (`passive`, `canonical_name`,
  `numeric_host`, `numeric_service`) combinable with `operator|`.

`bnio::udp::make_resolve_query(host, service, protocol)` (or the
`std::uint16_t` port overload) builds a query with the UDP transport and the
protocol's address version already applied.

### `io_context` construction and lifecycle

`bnio::io_context_options` controls context creation:

```cpp
struct io_context_options {
  std::uint32_t concurrency_hint = 1;   // advisory; run() workers register lazily
  bool enable_immediate_io = true;      // eager non-blocking probe; read at construction
  platform_io_context_options platform{}; // io_uring_ or kqueue_context_options
};
```

Pass it to `bnio::io_context ctx(options);`. `concurrency_hint` is advisory
and does not pre-create threads. `enable_immediate_io` is immutable after
construction and is also queryable through `ctx.enable_immediate_io()`.

Lifecycle and introspection methods:

| Member | Meaning |
|--------|---------|
| `is_open()` | Always true for the compiled POSIX backend; native backend failure is reported by `run()`, not here. |
| `run()` | Runs the loop on the calling thread. Empty on a normal run, including one that ends because of a stop; `operation_canceled` when entry is rejected after a stop request (published work is drained first); otherwise the backend entry error. |
| `request_stop()` | Phase-1 stop request: records the stop and broadcasts the wake channel. Takes no lock, aborts no timer, publishes no stopping state, drains nothing, and waits for nothing. Idempotent and the only context operation valid in an async signal handler. |
| `stop()` | Phase-1 request plus, when no worker is running, synchronous Phase 2 and an inline shared-queue drain. With workers it returns immediately and is not a barrier. Not async-signal-safe. |
| `join()` | Barrier sender: requests stop, drives Phase 2 itself, waits for every `run()` to return, drains the shared queues, and completes with `set_value()`. |
| `is_in_context()` | Whether the calling thread is running this context. |
| `is_stopped()` | True immediately after `request_stop()` or `stop()`, or once the Phase-2 transition has begun. |
| `get_post_scheduler()` / `get_dispatch_scheduler()` / `get_defer_scheduler()` | Scheduler handles described above. |

`request_stop()` is the signal-handler entry point: it is lock-free and
limited to setting the stop request and broadcasting the wake channel. It
does not abort timers or wait for `run()` to return. The context must
outlive any signal handler that can call it: destruction closes the shared
wake channel, so a handler racing the destructor is unsupported. `stop()` is
safe to call from a receiver callback — it returns without waiting — but it
is not async-signal-safe. Neither call by itself guarantees that all
completions have been delivered; use `join()` (or join the `run()` thread)
before destroying the context and whenever every completion must be
observed.

`join()` is the only barrier. It completes Phase 2 itself, waits for all
workers to exit, and then drains the shared queues; concurrent callers are
safe and every operation is still delivered exactly once. Do not start a
`join()` sender on a thread that is running this context, nor from a receiver
delivered by a stop/join drain: such a call waits for itself and never
completes.

## 4. Express scheduler capability with concepts

Generic code should usually talk about capabilities instead of concrete
scheduler types. bnio provides CPOs and concepts (available through
`bnio/bnio.h`) for that purpose.

The CPOs include:

```cpp
bnio::async_read(scheduler, source, buffer);
bnio::async_read_some(scheduler, source, buffer);
bnio::async_write(scheduler, sink, buffer);
bnio::async_write_some(scheduler, sink, buffer);
bnio::async_accept(scheduler, acceptor);
bnio::async_connect(scheduler, stream, endpoint);
bnio::async_poll(scheduler, descriptor, poll_mask);
bnio::async_resolve(scheduler, query, result_view);

bnio::ssl::async_handshake(scheduler, stream, handshake_type);
bnio::ssl::async_shutdown(scheduler, stream);
```

For stream objects, the CPO calls the stream customization first, such as
`stream.async_read(scheduler, buffer)`. For non-owning platform views, it falls
back to the scheduler member function. The `ssl::` CPOs forward to the SSL
stream's `async_handshake()` / `async_shutdown()` members and are kept for
generic code that treats TLS streams like any other stream. Scheduling is
expressed with `bexec::schedule(scheduler)`, not with a bnio CPO.

The matching concepts describe the operations a scheduler can provide:

| Concept | Meaning |
|---------|---------|
| `bexec::scheduler<Scheduler>` | `Scheduler` provides the standard scheduler interface. |
| `bnio::reads_bytes<Scheduler, Source, Buffer>` | `async_read(scheduler, source, buffer)` returns a sender. |
| `bnio::writes_bytes<Scheduler, Sink, Buffer>` | `async_write(scheduler, sink, buffer)` returns a sender. |
| `bnio::accepts_connections<Scheduler, Acceptor>` | `async_accept(scheduler, acceptor)` returns a sender. |
| `bnio::connects_stream<Scheduler, Stream, Endpoint>` | `async_connect(scheduler, stream, endpoint)` returns a sender. |
| `bnio::polls_descriptor<Scheduler, Descriptor>` | `async_poll(scheduler, descriptor, mask)` returns a sender. |
| `bnio::resolves_dns<Scheduler, Query>` | `async_resolve(scheduler, query, result_view)` returns a sender. |

That lets application code express requirements directly:

```cpp
template <class Scheduler, class Stream>
  requires bnio::reads_bytes<Scheduler, Stream, bnio::mutable_buffer> &&
           bnio::writes_bytes<Scheduler, Stream, bnio::const_buffer>
auto echo_once(Scheduler scheduler, Stream& stream,
               bnio::mutable_buffer input, bnio::const_buffer output) {
  auto read_sender = bnio::async_read(scheduler, stream, input);
  auto write_sender = bnio::async_write(scheduler, stream, output);

  // Compose or connect the returned senders as needed.
  (void)read_sender;
  (void)write_sender;
}
```

This pattern works for `tcp_socket`, `local::stream_socket`,
`ssl::tcp::stream`, descriptor views, and any future type that satisfies the
same CPO contract.

## 5. Passive I/O submission

All I/O factories use one passive submission path:

```cpp
auto read = socket.async_read(scheduler, bnio::buffer(bytes));
auto write = socket.async_write(scheduler, bnio::buffer(payload));
```

Operations enter an I/O queue: the publishing worker's own queue when the
operation is started from a callback running on that worker, otherwise the
shared, lower-priority I/O queue. A worker always handles CPU work first, then
takes every I/O operation currently published — its own queue first, the shared
queue next — and prepares the SQEs on its own io_uring. There is no
queue-length threshold, manual flush, queue-size query, or `_direct` variant.

Under concurrency, operations accumulate naturally while a worker handles CPU
work and the preceding batch. Under light load, the worker's pre-sleep handshake
rechecks and drains the I/O queue before it waits on eventfd. A producer that
publishes after the worker announces sleep wakes one worker, so a lone operation
does not depend on a timer or a configured batch size.

Submission policy is therefore no longer part of the API. The semantic suffixes
remain: `async_write()` is write-all, while `async_write_some()` performs one
write attempt. The same rule applies to TCP, UDP, descriptors, and TLS.

## 6. Coroutine support through bexec senders

bnio's coroutine examples are adapters over ordinary senders, not a separate
async API. The bnio throughput benchmark shows the pattern in
[`../../benchmarks/throughput/bnio_throughput_benchmark.cpp`](../../benchmarks/throughput/bnio_throughput_benchmark.cpp).

The adapter connects a sender to a receiver, starts the operation when the
coroutine suspends, stores the completion, and resumes the awaiting coroutine.
This is a trimmed sketch for a one-value sender; the example file contains the
complete result storage:

```cpp
template <class Sender>
class sender_awaiter {
 public:
  using sender_type = std::remove_cvref_t<Sender>;

  class receiver {
   public:
    explicit receiver(sender_awaiter& awaiter) noexcept : awaiter_(&awaiter) {}

    void set_value(auto value) noexcept {
      awaiter_->store_value(std::move(value));
      awaiter_->resume();
    }

    void set_stopped() noexcept {
      awaiter_->store_stopped();
      awaiter_->resume();
    }

   private:
    sender_awaiter* awaiter_;
  };

  using operation_type =
      decltype(bexec::connect(std::declval<sender_type&&>(),
                              std::declval<receiver>()));

  explicit sender_awaiter(Sender&& sender)
      : operation_(bexec::connect(std::forward<Sender>(sender),
                                  receiver(*this))) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> continuation) noexcept {
    continuation_ = continuation;
    bexec::start(operation_);
  }

  auto await_resume() noexcept {
    return take_result();
  }

 private:
  void resume() noexcept { continuation_.resume(); }

  std::coroutine_handle<> continuation_;
  operation_type operation_;
};
```

The important properties are the same as for manual sender/receiver usage:

- The underlying operation is still lazy until `bexec::start()` is called.
- The receiver still receives `set_value` or `set_stopped` — the adapter only
  handles these two completion channels.
- The operation state is stored inside the awaiter and must remain alive until
  completion.
- Any compatible bnio sender can be adapted this way with an awaiter that
  matches its `set_value` shape, including TCP, TLS, DNS, polling, timers, and
  scheduler operations.

Using the adapter makes coroutine code read linearly while preserving the
sender model:

```cpp
auto result =
    co_await async_result(socket.async_read(scheduler, bnio::buffer(bytes)));

if (!result || result.value() == 0) {
  co_return;
}
```

Coroutine support is therefore a convenience layer built with bexec's ordinary
`connect` and `start` operations. The same completion signatures, cancellation
behavior, and submission modes apply.
