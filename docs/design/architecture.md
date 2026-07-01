# Architecture

`bupp` is organized into three abstraction layers. Each layer has a distinct
responsibility, and layers depend strictly downward — higher layers use lower
layers; lower layers never reference higher ones.

## Layer Overview

```mermaid
graph TB
    subgraph L3["Layer 3 — bupp::io_context"]
        C3_ctx["io_context<br/>(event loop + scheduler factory)"]
        C3_tcp["tcp_socket / tcp_acceptor<br/>(RAII fd owners)"]
        C3_ssl["ssl_context / ssl_stream<br/>(RAII SSL owners)"]
        C3_buf["mutable_buffer / const_buffer<br/>dynamic_string_buffer<br/>(non-owning views / adapters)"]
    end

    subgraph L2["Layer 2 — bupp::async_io"]
        C2_buf["buffer_view<br/>(non-owning)"]
        C2_desc["descriptor_view<br/>(non-owning)"]
        C2_stream["stream_socket_view<br/>(non-owning)"]
        C2_listen["listening_socket_view<br/>(non-owning)"]
        C2_ip["address / endpoint<br/>(value types)"]
        C2_native["linux_native::io_uring_*<br/>(platform operations)"]
    end

    subgraph L1["Layer 1 — bupp::base"]
        C1_ring["ring<br/>(RAII io_uring owner)"]
        C1_sqe["submission_queue_entry<br/>(non-owning SQE view)"]
        C1_cqe["completion_queue_entry<br/>(non-owning CQE view)"]
        C1_probe["probe / params<br/>(RAII / value types)"]
    end

    subgraph SYS["System"]
        S1["liburing / io_uring"]
        S2["Linux socket API"]
        S3["OpenSSL"]
    end

    L1 --> SYS
    L2 --> L1
    L3 --> L2
    C3_tcp -.->|"view()"| C2_stream
    C3_buf -.->|"view()"| C2_buf
```

## The Ownership Dimension

Each layer is further characterized by whether its types **own** resources or
are **non-owning references**:

| Layer | Non-owning Views | RAII Owners |
|-------|-----------------|-------------|
| Layer 1 (`base`) | `submission_queue_entry`, `completion_queue_entry` | `ring`, `probe` |
| Layer 2 (`async_io`) | `buffer_view`, `descriptor_view`, `stream_socket_view`, `listening_socket_view` | *(none — by design)* |
| Layer 3 (`io_context`) | `mutable_buffer`, `const_buffer`, `dynamic_string_buffer` | `tcp_socket`, `tcp_acceptor`, `ssl_context`, `ssl_stream`, `io_context` |

**Key invariant:** Layer 2 (`bupp::async_io`) deliberately contains **no RAII
owners**. Every type is either a non-owning view or a pure value type. This
keeps the layer reusable by any higher-level framework without hidden ownership
coupling.

## Layer Dependency Graph

```mermaid
graph LR
    L3["io_context"] -->|"uses"| L2["async_io"]
    L2 -->|"uses"| L1["base"]
    L1 -->|"uses"| LIB["liburing"]
    L3 -->|"uses"| SSL["OpenSSL"]
    L2 -->|"uses"| LIB2["liburing<br/>(linux_native operations)"]
```

---

## Layer 1: `bupp::base` — Thin `liburing` Wrappers

Namespace `bupp::base`. Headers in `include/bupp/base/linux/`.

Maps the `liburing` C API to C++ objects with RAII where appropriate. Method
names drop the `io_uring_` prefix (e.g. `io_uring_submit` →
`ring::submit()`).

### `ring` — io_uring Instance Owner

RAII wrapper around `struct io_uring`. **The sole owner of an `io_uring`
instance.**

```cpp
class ring {
public:
    ring() noexcept;
    ~ring() noexcept;                   // calls queue_exit()

    ring(const ring&) = delete;
    ring& operator=(const ring&) = delete;
    ring(ring&& other) noexcept;
    ring& operator=(ring&& other) noexcept;

    int queue_init(unsigned entries, unsigned flags = 0) noexcept;
    int queue_init_params(unsigned entries, params& p) noexcept;
    void queue_exit() noexcept;

    int submit() noexcept;
    int submit_and_wait(unsigned wait_nr) noexcept;

    [[nodiscard]] submission_queue_entry get_sqe() noexcept;
    int peek_cqe(completion_queue_entry& cqe) noexcept;
    int wait_cqe(completion_queue_entry& cqe) noexcept;
    int wait_cqe_timeout(completion_queue_entry& cqe,
                         __kernel_timespec* timeout) noexcept;
    void cqe_seen(completion_queue_entry cqe) noexcept;

    template <class Handler>
    unsigned consume_ready_cqes(unsigned max_count,
                                Handler&& handler) noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] int native_fd() const noexcept;
};
```

### `submission_queue_entry` — Non-owning SQE View

Wraps `io_uring_sqe*`. The SQE memory lives in the ring's submission queue.
Copying this object copies only the pointer. Valid until `ring::submit()`
consumes the entry.

### `completion_queue_entry` — Non-owning CQE View

Wraps `io_uring_cqe*`. The CQE slot is recycled after `ring::cqe_seen()`.
Read all needed fields **before** calling `cqe_seen()`.

### Design Rules

Per [`maintaince.md`](../maintaince.md):

- Return semantics mirror `liburing`: ≥ 0 on success, negative `errno` on failure.
- No exceptions thrown.
- No executors, coroutines, schedulers, or higher-level async models.
- Base layer does **not** own fd, buffer, address, path, or message lifetimes
  unless a type explicitly documents ownership.

---

## Layer 2: `bupp::async_io` — Platform-Neutral Vocabulary Types

Namespace `bupp::async_io`. Headers in `include/bupp/async_io/`.

This layer defines **vocabulary types only**: non-owning views and value types.
It intentionally contains **zero RAII owners**.

### Non-Owning Views

#### `buffer_view`

```cpp
struct buffer_view {
    void* data = nullptr;
    std::size_t size = 0;
};
```

Plain aggregate. Copy copies pointer and size; the data is not owned.

#### `descriptor_view`

Non-owning wrapper around a native file descriptor (`int`). Destruction does
**not** close the descriptor.

#### Socket View Family

Role-specialized non-owning socket views built on `socket_view`:

| Type | Role | Extra Operations |
|------|------|-----------------|
| `socket_view` | Generic socket | — |
| `listening_socket_view` | Passive socket | `bind()`, `listen()`, `shutdown()`, `set_reuse_address()` |
| `stream_socket_view` | Active socket | `connect()`, `shutdown()`, `set_reuse_address()` |

Views can be constructed from each other (e.g. `socket_view` →
`listening_socket_view`). All hold the same fd value — only the exposed
operation set differs.

### Value Types

| Type | Description |
|------|-------------|
| `async_io::ip::address` | IPv4 or IPv6 address |
| `async_io::ip::endpoint` | IP address + port number |
| `async_io::ip::tcp` | TCP protocol tag (v4/v6/any) |
| `async_io::ip::udp` | UDP protocol tag |

These are copyable, self-contained value types.

### `linux_native::io_uring_context` — Platform Operation Context

Within `bupp::async_io::linux_native`, `io_uring_context` owns a `base::ring`
and provides an event loop with intrusive operation scheduling:

```cpp
class io_uring_context {
public:
    io_uring_context() noexcept;
    explicit io_uring_context(const io_uring_context_options& opts) noexcept;
    ~io_uring_context() noexcept;

    // non-copyable, non-movable

    int queue_init(const io_uring_context_options& opts) noexcept;
    void queue_exit() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    template <class Operation>
    int prepare(Operation& op) noexcept;        // lock, get SQE, fill

    int submit() noexcept;

    template <class Operation>
    int submit(Operation& op) noexcept;          // prepare + submit

    template <class Function>
    void submit_batch(Function&& fn) noexcept;   // batch prepare+submit

    int post(io_uring_operation_base& op) noexcept;
    void run() noexcept;
    int stop() noexcept;
    [[nodiscard]] bool is_in_context() const noexcept;
};
```

All operations derive from `io_uring_operation_base`:

```cpp
class io_uring_operation_base {
public:
    io_uring_operation_base* next = nullptr;
    int result = 0;
    unsigned flags = 0;

    // non-copyable, non-movable (intrusive list node)

    virtual ~io_uring_operation_base() = default;
    virtual void execute() noexcept = 0;
};
```

---

## Layer 3: `bupp::io_context` — High-Level Async I/O Context

Namespace `bupp`. Header `include/bupp/linux/io_context.h`.

`io_context` is the event-loop owner and scheduler factory:

1. **Event loop host** — `run()` drives the io_uring completion loop.
2. **Scheduler factory** — produces dispatch and post schedulers.
3. **I/O batching backend** — manages queued vs. direct submission for scheduler
   I/O senders.

### Submission Modes

```mermaid
graph LR
    subgraph Q["queued"]
        Q1["operation"] --> Q2["enqueue_io()"]
        Q2 --> Q3["pending_io list"]
        Q3 --> Q4["flush (count/timer/manual)"]
        Q4 --> Q5["batched io_uring submit"]
    end

    subgraph D["direct"]
        D1["operation"] --> D2["submit_direct()"]
        D2 --> D3["immediate io_uring submit"]
    end
```

| Mode | API Suffix | Trigger | Use Case |
|------|-----------|---------|----------|
| **queued** | `async_receive()`, etc. | Count (64), timer (1 ms), or manual `flush_io_queue()` | High throughput |
| **direct** | `async_receive_direct()`, etc. | Immediate | Low latency |

### Configuration

```cpp
struct linux_io_context_options {
    async_io::linux_native::io_uring_context_options uring{};
    std::size_t max_queued_io_operations = 64;
    async_io::duration queued_io_flush_after = std::chrono::milliseconds(1);
};

struct io_context_options {
    std::uint32_t concurrency_hint = 1;
    platform_io_context_options platform{};
};
```

### Sender Factories

Streams expose the high-level async I/O factories. Schedulers expose the
lowest-layer factories for socket views, file descriptors, polling, DNS, and
timers. Each factory returns a sender. Connecting a sender to a receiver and
calling `start()` begins the asynchronous I/O.

#### Stream Level

| Factory | Owner | `set_value` |
|---------|-------|-------------|
| `socket.async_receive(scheduler, buffer, flags)` | `tcp_socket` | `size_t` bytes received |
| `socket.async_send(scheduler, buffer, flags)` | `tcp_socket` | `size_t` bytes sent |
| `acceptor.async_accept(scheduler, flags)` | `tcp_acceptor` | `tcp_socket` new connection |
| `socket.async_connect(scheduler, endpoint)` | `tcp_socket` | `()` |
| `stream.async_handshake(scheduler, type)` | `ssl_stream` | `()` |
| `stream.async_receive(scheduler, buffer, flags)` | `ssl_stream` | `size_t` |
| `stream.async_send(scheduler, buffer, flags)` | `ssl_stream` | `size_t` |
| `stream.async_shutdown(scheduler)` | `ssl_stream` | `()` |

#### Scheduler Level

| Factory | Lowest-Layer Parameter | `set_value` |
|---------|------------------------|-------------|
| `scheduler.async_receive(view, buffer, flags)` | `stream_socket_view` | `size_t` bytes received |
| `scheduler.async_send(view, buffer, flags)` | `stream_socket_view` | `size_t` bytes sent |
| `scheduler.async_accept(view, flags)` | `listening_socket_view` | native fd |
| `scheduler.async_connect(view, endpoint)` | `stream_socket_view` | `()` |
| `scheduler.async_read(descriptor, buffer, offset)` | `descriptor_view` | `size_t` bytes read |
| `scheduler.async_write(descriptor, buffer, offset)` | `descriptor_view` | `size_t` bytes written |
| `scheduler.async_poll(descriptor, mask)` | `descriptor_view` | `unsigned` ready-event mask |

All senders also complete with `set_error(std::error_code)` or `set_stopped()`.

### Operation Flow Through Layers

```mermaid
sequenceDiagram
    participant User
    participant Ctx as io_context
    participant S as scheduler
    participant Stream as tcp_socket
    participant Op as operation
    participant UCtx as io_uring_context
    participant Ring as base::ring
    participant K as Kernel

    User->>Ctx: get_post_scheduler()
    Ctx-->>User: scheduler
    User->>Stream: async_receive(scheduler, buffer, flags)
    Stream-->>User: sender

    User->>Op: connect(receiver) → start()
    Op->>Ctx: enqueue_io(*this)

    Note over Ctx: flush triggers (count / timer / manual)
    Ctx->>Ctx: take_pending_io()
    Ctx->>UCtx: submit_batch(fn)
    UCtx->>Ring: get_sqe() + prepare(sqe)
    Ring->>K: io_uring_submit()

    Note over K: async I/O ...

    K-->>Ring: CQE ready
    UCtx->>Ring: collect_ready_cqes()
    UCtx->>Op: (result = res), execute()
    Op-->>User: set_value(receiver, bytes)
```

### Sender/Receiver Model & CPOs

`bupp` uses `bexec`'s sender/receiver model. Customization Point Objects
(CPOs) enable generic code to call async operations on any provider:

```cpp
// Direct stream call:
auto scheduler = ctx.get_post_scheduler();
auto s = socket.async_receive(scheduler, buffer, 0);

// Direct lowest-layer call:
auto low = scheduler.async_receive(socket.view(), buffer, 0);

// Through CPO (generic):
auto s = bupp::async_receive(scheduler, socket, buffer);
```

| CPO | Invokes | Header |
|-----|---------|--------|
| `bupp::async_receive` | `stream.async_receive(provider, buf)` or lowest-layer fallback | `io_context_cpo.h` |
| `bupp::async_send` | `stream.async_send(provider, buf)` or lowest-layer fallback | `io_context_cpo.h` |
| `bupp::async_accept` | `acceptor.async_accept(provider)` or lowest-layer fallback | `io_context_cpo.h` |
| `bupp::async_connect` | `stream.async_connect(provider, ep)` or lowest-layer fallback | `io_context_cpo.h` |
| `bupp::async_read` | `provider.async_read(descriptor, buf, offset)` | `io_context_cpo.h` |
| `bupp::async_write` | `provider.async_write(descriptor, buf, offset)` | `io_context_cpo.h` |
| `bupp::async_poll` | `provider.async_poll(descriptor, mask)` | `io_context_cpo.h` |
| `bupp::async_handshake` | `stream.async_handshake(provider, type)` | `ssl.h` |
| `bupp::async_shutdown` | `stream.async_shutdown(provider)` | `ssl.h` |

Each also has a `*_direct` variant (e.g. `async_receive_direct`).

Provider concepts:

```cpp
template <class Provider, class Stream, class Buffer>
concept receives_bytes =
    requires(Provider& p, Stream& s, Buffer&& b) {
        { async_receive(p, s, std::forward<Buffer>(b)) } -> bexec::sender;
    };

template <class Provider, class Stream, class Buffer>
concept sends_bytes = /* ... */;

template <class Provider, class Acceptor>
concept accepts_connections = /* ... */;

template <class Provider, class Descriptor, class Buffer>
concept reads_descriptor = /* ... */;

template <class Provider, class Descriptor, class Buffer>
concept writes_descriptor = /* ... */;
```

---

## Header Dependency Graph

```
bupp/bupp.h  (umbrella)
├── bupp/base.h
│   └── base/linux/{ring, submission_queue_entry, completion_queue_entry,
│                   params, probe}.h
├── bupp/async_io.h
│   ├── async_io/{buffer_view, descriptor_view, socket_view, time}.h
│   └── async_io/ip/{address, endpoint, tcp, udp}.h
├── bupp/io_context.h
│   └── linux/io_context.h
│       └── async_io/linux/io_uring_context.h
│           ├── async_io/linux/io_uring_context_base.h
│           └── async_io/linux/io_uring_operations.h
├── bupp/ip.h
├── bupp/buffer.h
├── bupp/tcp.h
├── bupp/ssl.h
├── bupp/io_context_cpo.h
└── bupp/export.h
```

For smaller translation units, include individual sub-headers (e.g.
`bupp/base.h`) instead of `bupp/bupp.h`.

## Namespace Map

```
bupp
├── base                                  Layer 1: liburing wrappers
│   ├── ring                              io_uring RAII owner
│   ├── submission_queue_entry            io_uring_sqe* non-owning view
│   ├── completion_queue_entry            io_uring_cqe* non-owning view
│   ├── params                            io_uring_params value type
│   └── probe                             io_uring_probe RAII owner
├── async_io                              Layer 2: vocabulary types
│   ├── buffer_view                       non-owning byte range
│   ├── descriptor_view                   non-owning fd
│   ├── socket_view                       non-owning generic socket
│   ├── listening_socket_view             non-owning passive socket
│   ├── stream_socket_view                non-owning active socket
│   ├── ip
│   │   ├── address                       IPv4/IPv6 address
│   │   ├── endpoint                      address + port
│   │   ├── tcp                           TCP protocol tag
│   │   └── udp                           UDP protocol tag
│   └── linux_native                      Linux-specific operations
│       ├── io_uring_context              platform event-loop owner
│       ├── io_uring_operation_base       intrusive operation node
│       └── io_uring_*_operation          concrete I/O operations
├── io_context                            Layer 3: high-level async context
│   └── operation_base                    intrusive queued-I/O node
├── ip
│   ├── address (= async_io::ip::address)
│   ├── endpoint (= async_io::ip::endpoint)
│   ├── tcp                               protocol tag + socket type aliases
│   └── udp (= async_io::ip::udp)
├── tcp_socket                            RAII TCP stream socket owner
├── tcp_acceptor                          RAII TCP listening socket owner
├── ssl_context                           RAII SSL_CTX owner
├── ssl_stream<NextLayer>                 RAII SSL + BIO + transport owner
├── mutable_buffer                        non-owning mutable byte view
├── const_buffer                          non-owning const byte view
├── dynamic_string_buffer                 dynamic buffer adapter (std::string)
└── dynamic_byte_vector_buffer<A>         dynamic buffer adapter (std::vector<std::byte>)
```
