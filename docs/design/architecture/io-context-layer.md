# Layer 3: `bnio::io_context` — High-Level Async I/O Context

Namespace `bnio`. Public umbrella `include/bnio/io_context.h` exposes one
platform-neutral class definition. `detail::native_context` aliases the
configured `io_uring_context` or `kqueue_context`; the shared runtime state,
worker ownership, timers, and source implementation live under
`detail/posix/io_context/` and `src/posix/io_context*.cpp`.

`io_context` is the event-loop owner and scheduler factory:

1. **Event loop host** — `run()` drives the selected io_uring or kqueue loop.
   Each thread calling `run()` creates a native context directly (without the
   intermediate `native_worker` wrapper that was removed in 0.0.4).
2. **Scheduler factory** — produces dispatch and post schedulers.
3. **Passive I/O backend** — publishes scheduler I/O to a shared queue that a
   worker drains on its owning native-context thread.

The public class is intentionally a coordinator. It owns a small set of
cohesive `detail` state objects rather than defining all internal data inline:

| State / Detail Type | Header | Responsibility |
|---------------------|--------|----------------|
| `detail::native_context` and related aliases | `detail/posix/io_context/native_context.h` | Select the native context, options, operation bases, and shared task state. |
| `detail::native_context_state` | `detail/posix/io_context/state.h` | Native options used to create contexts lazily. |
| `detail::native_worker_state` | `detail/posix/io_context/state.h` | Atomically published head of the native-worker list. |
| `detail::native_worker` | `detail/posix/io_context/native_worker.h` | Per-run-thread owner of one native context. |
| platform task queue state | `async_io/{linux,bsd}/.../operation_base.h` | Shared CPU/I/O queues, passive-timer callback, awake-worker count, and worker-group stopping state (`life_state`). |
| `detail::timer_state_data` | `detail/posix/io_context/timer_types.h` | Intrusive timer heap/list and the non-blocking passive-timer callback state. |

The aggregate `detail/posix/io_context/native_io.h` is included after the complete
`io_context` declaration, so templates can call private context hooks without
splitting a class definition across files. It owns the shared forwarding,
timer-wait, and write-all templates, then selects only the backend request
factories under `detail/{linux,bsd}/io_context_native_io/`. Those factories
intentionally retain the distinct readiness versus completion semantics of the
two native backends.

### Passive I/O Publication

```mermaid
graph LR
    I["operation"] --> E["publish_io()"]
    E --> Q["shared lower-priority I/O queue"]
    Q --> W["native context run loop takes all I/O"]
    W --> U["prepare SQEs or readiness registrations"]
```

There is one publication policy. Producers publish I/O and wake a worker when
the shared awake count indicates that a worker is sleeping. Workers give the
CPU queue priority, then atomically take the complete I/O list. Busy workloads
naturally form larger kernel submission batches; idle workloads reach the same
drain during the pre-sleep recheck. No count, threshold, explicit flush, or
direct variant is involved.

Layer 3 does not select or post through a specific native context. It wraps
platform request objects in high-level senders, publishes CPU or I/O work to
the shared queues, and lets whichever `run()` worker takes the work own the
native preparation. On BSD, each socket request first attempts its
nonblocking call and registers the matching kqueue filter only when it would
block. The request repeats the call after readiness. On Linux, immediate
attempts and SQE preparation likewise remain platform-native implementation
details.

BSD regular-file requests use a documented blocking-at-start fallback:
`start()` performs one positioned `pread()` or `pwrite()`, then posts the
receiver completion through the shared CPU queue. Thus data transfer is
finished when `start()` returns, but no receiver is called inline. This keeps
the public sender interface aligned with Linux without pretending that kqueue
provides asynchronous regular-file kernel work.

### Configuration

```cpp
using platform_io_context_options =
    /* io_uring_context_options or kqueue_context_options */;

struct io_context_options {
    std::uint32_t concurrency_hint = 1;
    platform_io_context_options platform{};
};
```

`concurrency_hint` is advisory; it does not reserve workers or create a native
context. Every call to `run()` constructs one native context, attaches the
shared group state, and inserts its worker at the head of the worker list.
This preserves **one thread, one uring/kqueue** without a construction-time
primary context. Work may be started before `run()` because it first enters the
shared queues.

When single-issuer mode is available, each lazily created Linux ring is
initially disabled. The same thread that calls its `run()` enables the ring
before preparing or submitting SQEs, becoming that ring's designated issuer.

High-level CPU work is published to the shared CPU queue. Wakeup scans the
head-linked worker list and signals one sleeping worker. I/O is published to
the lower-priority shared I/O queue. The worker that removes an I/O batch owns
all SQ preparation and submission for that batch, so high-level queue code
does not need native ring synchronization. Timer bookkeeping remains on the
high-level context, but native workers consume its deadline passively while
choosing their blocking timeout. Timer-ready completions bypass the shared CPU
queue: the worker that performs the timer check links them directly into its
local CPU queue. No reusable timer SQE or timer-update request exists.

The shared state is owned by `io_context`, not by any native context. Worker
registration calls `set_global_state()` before publishing the new worker at the
list head. `run()` uses that state for CPU work, I/O work, the passive timer
callback, the awake-worker count, and the group `life_state` flag (0=running, 1=stopping). A standalone
native context must also receive externally owned state before `run()`; no
hidden shared-state fallback is allocated.

`io_context::stop()` atomically transitions the shared `life_state` from 0
(running) to 1 (stopping) via CAS before calling `stop_internal()`, which
aborts pending timer waits and wakes every native worker via the shared
wake channel. Worker registration and the `run()` path both check
`life_state` before and after publishing a new slot, so a worker that races
with the stop transition cannot enter a new idle run loop.

Before a worker blocks, it publishes sleeping in two stages: first its local
waiting flag, then a decrement of the shared awake-worker count. It then
rechecks completions, CPU work, I/O work, and the passive timer heap. The
timer check is guarded by an atomic admission flag so only one worker attempts
the timer mutex at a time. Finding any work reopens the worker and starts
another loop pass. Otherwise it blocks until native I/O, an explicit wakeup,
or the timer heap's next deadline. This handshake replaces both the former
queued-I/O flush timer and active timer submission.

### Sender Factories

Streams expose the high-level async I/O factories. Schedulers expose the
lowest-layer factories for socket views, file descriptors, polling, DNS, and
timers. Each factory returns a sender. Connecting a sender to a receiver and
calling `start()` begins the asynchronous I/O.

#### Stream Level

The `set_value` column lists only the success payload. Every sender's actual
signature is `set_value(std::error_code, <payload>)` — the leading `ec` is
empty on success and carries a recoverable error or `operation_canceled`
otherwise. See the note below the Scheduler Level table for the full contract.

| Factory | Owner | `set_value` (success payload) |
|---------|-------|-------------------------------|
| `socket.async_read(scheduler, buffer, flags)` | `tcp_socket` | `size_t` total bytes read until buffer full or EOF |
| `socket.async_read_some(scheduler, buffer, flags)` | `tcp_socket` | `size_t` bytes read by one operation |
| `socket.async_write(scheduler, buffer, flags)` | `tcp_socket` | `size_t` total bytes written |
| `socket.async_write_some(scheduler, buffer, flags)` | `tcp_socket` | `size_t` bytes written by one operation |
| `acceptor.async_accept(scheduler, flags)` | `tcp_acceptor` | `tcp_socket` new connection |
| `socket.async_connect(scheduler, endpoint)` | `tcp_socket` | `()` |
| `socket.async_send_to(scheduler, buffer, endpoint, flags)` | `udp::socket` | one datagram byte count |
| `socket.async_receive_from(scheduler, buffer, endpoint, flags)` | `udp::socket` | one datagram byte count |
| `socket.async_send(scheduler, buffer, flags)` | connected `udp::socket` | one datagram byte count |
| `socket.async_receive(scheduler, buffer, flags)` | connected `udp::socket` | one datagram byte count |
| `stream.async_handshake(scheduler, type)` | `ssl_stream` | `()` |
| `stream.async_read(scheduler, buffer, flags)` | `ssl_stream` | `size_t` plaintext bytes read by one operation |
| `stream.async_read_some(scheduler, buffer, flags)` | `ssl_stream` | `size_t` plaintext bytes read by one operation |
| `stream.async_write(scheduler, buffer, flags)` | `ssl_stream` | `size_t` total plaintext bytes written |
| `stream.async_write_some(scheduler, buffer, flags)` | `ssl_stream` | `size_t` plaintext bytes accepted by one SSL write step |
| `stream.async_shutdown(scheduler)` | `ssl_stream` | `()` |

#### Scheduler Level

The `set_value` column lists only the success payload. The actual signature is
`set_value(std::error_code, <payload>)`.

| Factory | Lowest-Layer Parameter | `set_value` (success payload) |
|---------|------------------------|-------------------------------|
| `scheduler.async_read(view, buffer, flags)` | `stream_socket_view` | `size_t` total bytes read until buffer full or EOF |
| `scheduler.async_read_some(view, buffer, flags)` | `stream_socket_view` | `size_t` bytes read by one operation |
| `scheduler.async_write(view, buffer, flags)` | `stream_socket_view` | `size_t` total bytes written |
| `scheduler.async_write_some(view, buffer, flags)` | `stream_socket_view` | `size_t` bytes written by one operation |
| `scheduler.async_accept(view, flags)` | `stream_socket_view` | native fd |
| `scheduler.async_connect(view, endpoint)` | `stream_socket_view` | `()` |
| `scheduler.async_send(view, buffer, flags)` | `datagram_socket_view` | one datagram byte count |
| `scheduler.async_receive(view, buffer, flags)` | `datagram_socket_view` | one datagram byte count |
| `scheduler.async_send_to(view, buffer, endpoint, flags)` | `datagram_socket_view` | one datagram byte count |
| `scheduler.async_receive_from(view, buffer, endpoint, flags)` | `datagram_socket_view` | one datagram byte count |
| `scheduler.async_read(descriptor, buffer, offset)` | `descriptor_view` | `size_t` total bytes read until buffer full or EOF |
| `scheduler.async_read_some(descriptor, buffer, offset)` | `descriptor_view` | `size_t` bytes read by one operation |
| `scheduler.async_write(descriptor, buffer, offset)` | `descriptor_view` | `size_t` total bytes written |
| `scheduler.async_write_some(descriptor, buffer, offset)` | `descriptor_view` | `size_t` bytes written by one operation |
| `scheduler.async_poll(descriptor, mask)` | `descriptor_view` | `unsigned` ready-event mask |

All senders complete with `set_value(std::error_code, ...)` as the universal
observable exit: the leading `ec` distinguishes success (`ec == {}`),
recoverable failure (`ec == <errno-derived>`), and user stop-token
cancellation (`ec == operation_canceled`). `set_stopped()` is emitted
exclusively by `io_context::stop()` aborting an inflight operation. The bnio
sender/receiver contract uses only these two completion channels; `set_error`
is not part of it — no bnio `completion_signatures` include it, and no bnio
receiver implements it.

### Internal Header Layout

The shared `io_context` layer keeps one class declaration and one source
implementation:

| Header | Contents |
|--------|----------|
| `io_context.h` | Public `io_context` umbrella. |
| `detail/posix/io_context/class.h` | `io_context`, schedulers, operation base, and private hooks. |
| `detail/posix/io_context/native_context.h` | Backend-selected native type aliases. |
| `detail/posix/io_context/{options,state,native_worker}.h` | Options, shared worker list, and per-thread native owner. |
| `detail/posix/io_context/{timer_types,steady_timer}.h` | Timer slots, queues, state, and public `steady_timer`. |
| `detail/posix/io_context/native_io.h` | Shared scheduler/context forwarding functions; selects backend request factories. |
| `detail/posix/io_context/{timer_wait,write_all}.h` | Shared timer wait and composed full-write sender templates. |
| `detail/{linux,bsd}/io_context_native_io/` | Backend-specific request adapters, factories, and (on Linux) SQE models. |
| `src/posix/io_context{,_queue,_timer,_timer_state}.cpp` | Shared lifecycle, queue, timer, and timer-state implementations. |

The request adapters are deliberately not macro-normalized: kqueue owns a
readiness/retry step while io_uring owns SQE preparation and CQE completion.
Detail headers are installable because public inline templates reference them.
User code should normally include `bnio/io_context.h`, not the detail headers
directly.

### Read/Write Semantic Split

The I/O API intentionally separates a native-attempt operation from a composed
full-write operation:

- `async_read()` and `async_read_some()` both mean "one read attempt". They
  complete after one bounded kernel receive/read or one SSL plaintext read step.
  Callers that need an exact byte count build their own parser/state loop.
- `async_write_some()` means "one write attempt". It preserves native short
  write behavior and reports the bytes accepted by that attempt.
- `async_write()` means "write the whole supplied buffer". It composes
  `async_write_some()` in a loop and reports the total bytes written.

The split exists because io_uring SQEs carry a kernel-sized length field while
public buffers are `std::size_t`. Every one-attempt operation bounds the native
request length before preparing the SQE. Write-all operations then repeat those
bounded attempts until the public buffer is exhausted.

#### Write-All as `state + repeat_until`

Write-all is implemented as a sender adaptor, not as a special io_uring
operation. The operation owns a small state object plus a `repeat_until`
operation. Each repeat iteration creates a fresh child sender for the current
slice:

```mermaid
stateDiagram-v2
    [*] --> Start
    Start --> Done: buffer.size == 0
    Start --> SubmitSome: remaining > 0
    SubmitSome --> Advance: set_value(empty ec, n > 0)
    Advance --> Done: transferred == buffer.size
    Advance --> SubmitSome: transferred < buffer.size
    SubmitSome --> ValueError: set_value(ec, bytes) — recoverable failure or cancel
    SubmitSome --> Stopped: set_stopped() — io_context::stop() only
    SubmitSome --> ValueError: set_value(ec, 0) — EOF / invariant violation
    Done --> [*]: set_value(empty ec, total)
    ValueError --> [*]: set_value(ec, transferred)
    Stopped --> [*]: set_stopped()
```

The state contains:

| Field | Purpose |
|-------|---------|
| scheduler/context pointer | Creates each next child sender against the same provider. |
| sink handle | `stream_socket_view` or `descriptor_view`. |
| buffer | Original non-owning `const_buffer`; caller storage must outlive the composed operation. |
| flags or offset | Socket flags, or descriptor starting offset. |
| transferred | Number of bytes successfully written so far. |
| done | Predicate observed by `repeat_until` after each child completion. |

For descriptor writes, each child uses `offset + transferred`, so retries write
the next file range. For socket writes, each child advances the buffer pointer
by `transferred`. A child result of zero before completion is treated as an
error to avoid an infinite repeat loop.

This design keeps each native I/O operation simple and single-purpose while
making full-write behavior explicit, testable, and reusable. Every child
attempt uses `async_write_some()` and enters the same passive I/O path.

#### SSL Use of the Same Pattern

`ssl_stream` already drives OpenSSL through a state machine. The transport side
now always uses the lower layer's `async_read_some*` and `async_write_some*`
because BIO flush/refill operations need native-attempt semantics. Plaintext
`ssl_stream::async_write()` uses the same repeat idea at the SSL layer: it
tracks plaintext bytes accepted by `SSL_write`, flushes encrypted BIO output,
and repeats until the whole plaintext buffer is accepted and flushed.

`ssl_stream::async_write_some()` is the escape hatch for callers that want one
SSL write step and their own retry policy. `ssl_stream::async_read()` remains a
read-some operation because TLS records and application protocol frames do not
map cleanly to a caller's buffer size.

### Operation Flow Through Layers

```mermaid
sequenceDiagram
    participant User
    participant Ctx as io_context
    participant S as scheduler
    participant Stream as tcp_socket
    participant Op as operation
    participant Worker as native_worker (slot)
    participant UCtx as io_uring_context
    participant Ring as base::ring
    participant K as Kernel

    User->>Ctx: get_post_scheduler()
    Ctx-->>User: scheduler
    User->>Stream: async_read(scheduler, buffer, flags)
    Stream-->>User: sender

    User->>Op: connect(receiver) → start()
    Op->>Ctx: publish_io(*this)

    Note over Ctx: publish to shared lower-priority I/O queue
    Ctx->>Worker: notify one worker if sleeping
    Worker->>UCtx: run(): CPU queue first
    UCtx->>UCtx: consume_io_tasks(): global_state_->pop_io_all()
    UCtx->>Ring: get_sqe() + operation.prepare(sqe)
    Ring->>K: io_uring_submit()

    Note over K: async I/O ...

    K-->>Ring: CQE ready
    UCtx->>Ring: collect_ready_cqes()
    UCtx->>Op: (result = res), execute()
    Op-->>User: set_value(receiver, ec, bytes)
```

### Sender/Receiver Model & CPOs

`bnio` uses `bexec`'s sender/receiver model. Customization Point Objects
(CPOs) enable generic code to call async operations on any provider:

```cpp
// Stream call:
auto scheduler = ctx.get_post_scheduler();
auto s = socket.async_read(scheduler, buffer, 0);

// Lowest-layer call:
auto low = scheduler.async_read(socket.view(), buffer, 0);

// Through CPO (generic):
auto s = bnio::async_read(scheduler, socket, buffer);
```

| CPO | Invokes | Header |
|-----|---------|--------|
| `bnio::async_read(provider, stream, buf)` | `stream.async_read(provider, buf)` or lowest-layer fallback | `io_context_cpo.h` |
| `bnio::async_read_some(provider, stream, buf)` | `stream.async_read_some(provider, buf)` or lowest-layer fallback | `io_context_cpo.h` |
| `bnio::async_write(provider, stream, buf)` | `stream.async_write(provider, buf)` or lowest-layer fallback | `io_context_cpo.h` |
| `bnio::async_write_some(provider, stream, buf)` | `stream.async_write_some(provider, buf)` or lowest-layer fallback | `io_context_cpo.h` |
| `bnio::async_accept` | `acceptor.async_accept(provider)` or lowest-layer fallback | `io_context_cpo.h` |
| `bnio::async_connect` | `stream.async_connect(provider, ep)` or lowest-layer fallback | `io_context_cpo.h` |
| `bnio::async_read(provider, descriptor, buf, offset)` | `provider.async_read(descriptor, buf, offset)` | `io_context_cpo.h` |
| `bnio::async_read_some(provider, descriptor, buf, offset)` | `provider.async_read_some(descriptor, buf, offset)` | `io_context_cpo.h` |
| `bnio::async_write(provider, descriptor, buf, offset)` | `provider.async_write(descriptor, buf, offset)` | `io_context_cpo.h` |
| `bnio::async_write_some(provider, descriptor, buf, offset)` | `provider.async_write_some(descriptor, buf, offset)` | `io_context_cpo.h` |
| `bnio::async_poll` | `provider.async_poll(descriptor, mask)` | `io_context_cpo.h` |
| `bnio::async_handshake` | `stream.async_handshake(provider, type)` | `ssl.h` |
| `bnio::async_shutdown` | `stream.async_shutdown(provider)` | `ssl.h` |

Provider concepts:

```cpp
template <class Provider, class Source, class Buffer>
concept reads_bytes =
    requires(Provider& p, Source& s, Buffer&& b) {
        { async_read(p, s, std::forward<Buffer>(b)) } -> bexec::sender;
    };

template <class Provider, class Sink, class Buffer>
concept writes_bytes = /* ... */;

template <class Provider, class Acceptor>
concept accepts_connections = /* ... */;

// Descriptor I/O uses the same reads_bytes/writes_bytes concepts with
// async_io::descriptor_view as the source/sink type.
```

---
