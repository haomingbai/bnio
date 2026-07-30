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

A minimal receiver handles the three completion channels:

```cpp
struct read_receiver {
  void set_value(std::size_t n) noexcept {
    // n bytes were transferred.
  }

  void set_error(std::error_code ec) noexcept {
    // The operation failed.
  }

  void set_stopped() noexcept {
    // Cancellation was observed before completion.
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

`set_value` is used for successful completion, `set_error(std::error_code)` for
I/O failure, and `set_stopped()` for cancellation. Receivers may also expose
`get_env()` when they need to provide stop tokens or other receiver environment
queries.

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

All of these senders use `set_error(std::error_code)` for failures and
`set_stopped()` for stopped completion.

Read and write names are intentionally precise:

`async_read()` reads one available chunk and completes with that chunk size. It
may complete with fewer bytes than the buffer size, including `0` for EOF on
plain descriptors or TCP sockets.

`async_read_some()` is the explicit spelling for the same one-read behavior. It
is useful in generic code where the distinction from write-all APIs should be
visible.

`async_write()` is write-all for TCP streams, TLS streams, and file
descriptors. It repeats bounded native writes until the entire buffer is
transferred, or until an error or stopped completion occurs.

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

Operations enter the shared, lower-priority I/O queue. A worker always handles
CPU work first, then takes every I/O operation currently published and prepares
the SQEs on its own io_uring. There is no queue-length threshold, manual flush,
queue-size query, or `_direct` variant.

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

    void set_error(std::error_code ec) noexcept {
      awaiter_->store_error(ec);
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
- The receiver still receives `set_value`, `set_error`, or `set_stopped`.
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
