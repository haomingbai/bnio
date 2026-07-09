# Usage Guide

This guide introduces bupp in the order most users need it: first the
sender/receiver operation model, then the available operation families, then
the scheduler capability concepts used by generic code, followed by submission
mode choices and coroutine integration.

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
#include <bupp/bupp.h>
#include <bexec/bexec.hpp>

#include <array>

int main() {
  bupp::io_context ctx;
  auto scheduler = ctx.get_post_scheduler();

  bupp::tcp_socket socket;
  // Assume socket has already been opened and connected.

  std::array<char, 4096> bytes{};
  auto sender = socket.async_read(scheduler, bupp::buffer(bytes), 0);
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
bupp::io_context ctx;

auto post = ctx.get_post_scheduler();       // schedule() always posts.
auto dispatch = ctx.get_dispatch_scheduler(); // schedule() may run inline.
```

The scheduler is passed to stream factories such as
`socket.async_read(scheduler, buffer)`, and it also exposes lower-level
factories for non-owning views such as `descriptor_view` and
`stream_socket_view`.

## 2. Operation types

The high-level operation families are all ordinary senders. They differ only in
what work they start and what value they send on success.

| Operation family | Common APIs | Successful `set_value` |
|------------------|-------------|-------------------------|
| Scheduling | `bexec::schedule(scheduler)` | `()` |
| TCP accept | `tcp_acceptor::async_accept(...)` | `tcp_socket` |
| TCP connect | `tcp_socket::async_connect(...)` | `()` |
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

For lower-level scheduler/view APIs, the same sender model is used but the
successful value may be closer to the platform operation. For example,
`scheduler.async_accept(listening_socket_view, flags)` completes with a native
file descriptor, while `tcp_acceptor::async_accept(scheduler, flags)` wraps
that descriptor in a `tcp_socket`.

## 3. Express scheduler capability with concepts

Generic code should usually talk about capabilities instead of concrete
scheduler types. bupp provides CPOs and concepts in `bupp/io_context_cpo.h` for
that purpose.

The CPOs include:

```cpp
bupp::async_read(scheduler, source, buffer);
bupp::async_write(scheduler, sink, buffer);
bupp::async_accept(scheduler, acceptor);
bupp::async_connect(scheduler, stream, endpoint);
bupp::async_poll(scheduler, descriptor, poll_mask);
bupp::async_resolve(scheduler, query, result_view);
```

For stream objects, the CPO calls the stream customization first, such as
`stream.async_read(scheduler, buffer)`. For view types, it falls back to the
scheduler member, such as `scheduler.async_read(descriptor_view, buffer)`.

The matching concepts describe the operations a scheduler can provide:

| Concept | Meaning |
|---------|---------|
| `bexec::scheduler<Scheduler>` | `Scheduler` provides the standard scheduler interface. |
| `bupp::reads_bytes<Scheduler, Source, Buffer>` | `async_read(scheduler, source, buffer)` returns a sender. |
| `bupp::writes_bytes<Scheduler, Sink, Buffer>` | `async_write(scheduler, sink, buffer)` returns a sender. |
| `bupp::accepts_connections<Scheduler, Acceptor>` | `async_accept(scheduler, acceptor)` returns a sender. |
| `bupp::connects_stream<Scheduler, Stream, Endpoint>` | `async_connect(scheduler, stream, endpoint)` returns a sender. |
| `bupp::polls_descriptor<Scheduler, Descriptor>` | `async_poll(scheduler, descriptor, mask)` returns a sender. |
| `bupp::resolves_dns<Scheduler, Query>` | `async_resolve(scheduler, query, result_view)` returns a sender. |

That lets application code express requirements directly:

```cpp
template <class Scheduler, class Stream>
  requires bupp::reads_bytes<Scheduler, Stream, bupp::mutable_buffer> &&
           bupp::writes_bytes<Scheduler, Stream, bupp::const_buffer>
auto echo_once(Scheduler scheduler, Stream& stream,
               bupp::mutable_buffer input, bupp::const_buffer output) {
  auto read_sender = bupp::async_read(scheduler, stream, input);
  auto write_sender = bupp::async_write(scheduler, stream, output);

  // Compose or connect the returned senders as needed.
  (void)read_sender;
  (void)write_sender;
}
```

This pattern works for `tcp_socket`, `ssl_stream`, descriptor views, and any
future type that satisfies the same CPO contract.

## 4. Queued and direct submission

Most I/O factories use queued submission by default:

```cpp
auto read = socket.async_read(scheduler, bupp::buffer(bytes));
auto write = socket.async_write(scheduler, bupp::buffer(payload));
```

Queued operations enter the scheduler's queued I/O list, allowing the
`io_context` implementation to batch submissions to io_uring. This is the
default path and is usually the right choice for throughput-oriented servers.

Direct submission uses the `_direct` suffix:

```cpp
auto read = socket.async_read_direct(scheduler, bupp::buffer(bytes));
auto write = socket.async_write_direct(scheduler, bupp::buffer(payload));
```

Direct operations bypass the queued I/O batch and submit immediately through
the owning context. This can reduce latency when the caller wants a single
operation submitted now instead of batched with other pending I/O.

The suffix only changes submission mode. It does not change operation
semantics:

| Queued API | Direct API | Same semantic |
|------------|------------|---------------|
| `async_read()` | `async_read_direct()` | one read chunk |
| `async_read_some()` | `async_read_some_direct()` | one read chunk |
| `async_write()` | `async_write_direct()` | write-all |
| `async_write_some()` | `async_write_some_direct()` | one write attempt |
| `async_accept()` | `async_accept_direct()` | accept one connection |
| `async_connect()` | `async_connect_direct()` | connect one stream |
| `async_poll()` | `async_poll_direct()` | wait for descriptor events |

TLS direct operations keep their TLS-level behavior as well. For example,
`ssl_stream::async_write_direct()` is still a TLS write-all sender; the direct
part applies to the lower-level transport I/O used by the TLS state machine.

## 5. Coroutine support through bexec senders

bupp's coroutine examples are adapters over ordinary senders, not a separate
async API. The raw echo example shows the pattern in
[`../../examples/raw_echo/bupp_raw_echo.cpp`](../../examples/raw_echo/bupp_raw_echo.cpp).

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
- Any compatible bupp sender can be adapted this way with an awaiter that
  matches its `set_value` shape, including TCP, TLS, DNS, polling, timers, and
  scheduler operations.

Using the adapter makes coroutine code read linearly while preserving the
sender model:

```cpp
auto result =
    co_await async_result(socket.async_read(scheduler, bupp::buffer(bytes)));

if (!result || result.value() == 0) {
  co_return;
}
```

Coroutine support is therefore a convenience layer built with bexec's ordinary
`connect` and `start` operations. The same completion signatures, cancellation
behavior, and submission modes apply.
