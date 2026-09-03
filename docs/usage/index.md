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
`bnio::ip`, `bnio::tcp`, and `bnio::udp`). Internal namespaces such as
`bnio::async_io` and `bnio::base` are implementation details and should not be
used directly in application code.

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
| `io_context::stop()` aborting inflight I/O or not-yet-executed queued work | `set_value(operation_canceled, ...)` |
| `steady_timer::cancel()`, `expires_after()`, timer destruction | `set_value(operation_canceled)` |
| `async_wait()` on an unregistered timer (moved-from `steady_timer`, or waits aborted by `io_context::stop()`) | `set_value(operation_canceled)` |
| Kernel-level cancel (CQE / kevent reporting `ECANCELED`) | `set_value(ECANCELED, ...)` |

Rules that follow from the table:

- Both sources racing: when the stop token is cancelled and the context is
  stopping at the same time, the token wins — the operation completes with
  `set_stopped()`.
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
- `io_context::run()` keeps returning `operation_canceled` as its own
  function result after a stop; that channel is unchanged. When the native
  backend fails to enter its run loop on a worker — the io_uring ring could
  not be enabled or a wake poll could not be armed — that same result
  carries the backend's error instead: `std::error_code(-errno,
  std::generic_category())` from the failing enter step, with an empty
  error_code for a normal run. Whatever was already published when the
  enter failed is still delivered to its receivers before `run()` returns.
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
```

The scheduler is passed to stream factories such as
`socket.async_read(scheduler, buffer)`.

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
| Byte reads | `async_read(...)`, `async_read_some(...)` | `std::size_t` |
| Byte writes | `async_write(...)`, `async_write_some(...)` | `std::size_t` |
| Descriptor polling | `async_poll(...)` | `unsigned` ready mask |
| DNS resolution | `async_resolve(...)` | `std::size_t` result count |
| Timer wait | `steady_timer::async_wait()` | `()` |
| TLS handshake/shutdown | `ssl_stream::async_handshake(...)`, `async_shutdown(...)` | `()` |
| TLS reads/writes | `ssl_stream::async_read(...)`, `async_write(...)` | `std::size_t` |

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

TLS streams encode EOF and close results the same way as plain descriptors
and TCP sockets:

- `ssl_stream::async_read()` and `ssl_stream::async_read_some()` both mean
  one plaintext `SSL_read` attempt. There is no read-all form for TLS: TLS
  record boundaries do not map to a caller's buffer size, so a call reports
  the bytes of a single `SSL_read` step.
- An orderly TLS close (the peer sent `close_notify`) completes a read
  successfully with `set_value({}, 0)` — the same EOF encoding as the
  zero-byte read on a plain descriptor or TCP socket.
- A transport-level close without `close_notify` is a truncated stream: the
  read completes with `set_value(connection_reset, n)`, the same pass-through
  a TCP socket reports on `ECONNRESET`.
- `ssl_stream::async_write()` is write-all. If the peer disappears mid-write
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
completes with `bnio::make_no_ssl_error()`: a dedicated value in the OpenSSL
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

UDP operations always transfer exactly one datagram. `async_send_to()` and
`async_receive_from()` preserve message boundaries and never use stream-style
write-all retries. Call `udp::socket::connect(endpoint)` to set a default peer,
then use `async_send()` and `async_receive()`. The endpoint passed to
`async_receive_from()` is output storage and must remain alive until the sender
completes.

Use `udp::make_resolve_query(host, service, protocol)` to create a
`dns_query` with the UDP transport and address-family filters already set, then
pass it to the existing `async_resolve()` API.

The sender model is the same across all operation levels. For example,
`tcp_acceptor::async_accept(scheduler, flags)` completes with a `tcp_socket`,
which owns the underlying file descriptor so users do not need to manage raw
native handles.

## 3. Express scheduler capability with concepts

Generic code should usually talk about capabilities instead of concrete
scheduler types. bnio provides CPOs and concepts (available through
`bnio/bnio.h`) for that purpose.

The CPOs include:

```cpp
bnio::async_read(scheduler, source, buffer);
bnio::async_write(scheduler, sink, buffer);
bnio::async_accept(scheduler, acceptor);
bnio::async_connect(scheduler, stream, endpoint);
bnio::async_poll(scheduler, descriptor, poll_mask);
bnio::async_resolve(scheduler, query, result_view);
```

For stream objects, the CPO calls the stream customization first, such as
`stream.async_read(scheduler, buffer)`. For non-owning platform views, it falls
back to the scheduler member function.

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

This pattern works for `tcp_socket`, `ssl_stream`, descriptor views, and any
future type that satisfies the same CPO contract.

## 4. Passive I/O submission

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

## 5. Coroutine support through bexec senders

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
