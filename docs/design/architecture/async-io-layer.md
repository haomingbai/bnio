# Layer 2: `bupp::async_io` — Platform-Neutral Vocabulary Types

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

Socket-kind-specific non-owning views built on `socket_view`:

| Type | Native Kind | Operations |
|------|-------------|------------|
| `socket_view` | Unspecified | Descriptor access only |
| `stream_socket_view` | `SOCK_STREAM` | `bind()`, `listen()`, `connect()`, `shutdown()`, socket options |
| `datagram_socket_view` | `SOCK_DGRAM` | `bind()`, `connect()`, endpoint queries; transfer is async-only |

Typed views can be constructed from `socket_view` and all hold the same fd
value. Listening is a lifecycle state of a stream socket, not a separate
socket kind. Stream and datagram operations remain distinct so stream partial
I/O rules cannot be applied to a datagram.

### Value Types

| Type | Description |
|------|-------------|
| `async_io::ip::address` | IPv4 or IPv6 address |
| `async_io::ip::endpoint` | IP address + port number |
| `async_io::ip::tcp` | TCP protocol tag (v4/v6/any) |
| `async_io::ip::udp` | UDP protocol tag |

These are copyable, self-contained value types.

### `linux_native::io_uring_context` — Platform Event Loop

Within `bupp::async_io::linux_native`, `io_uring_context` owns a `base::ring`
and provides a single-threaded event loop. Under the **one-thread-one-uring**
model, each run-loop thread owns its own `io_uring_context` (allocated from
`io_context`'s native worker pool). Posted work is stored in a non-atomic
`operation_stack_state`; the async_io layer no longer provides an MPSC task
queue. Higher layers that accept cross-thread producers must synchronize and
batch work before handing it to a native context.

```cpp
class io_uring_context {
public:
    io_uring_context() noexcept;
    explicit io_uring_context(const io_uring_context_options& opts) noexcept;
    ~io_uring_context() noexcept;

    // non-copyable, non-movable (owns synchronization primitives
    // and thread-local run-loop state)

    int queue_init(const io_uring_context_options& opts) noexcept;
    void queue_exit() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] unsigned kernel_features() const noexcept;

    template <class Operation>
    int prepare(Operation& op) noexcept;        // lock, get SQE, fill

    int submit() noexcept;

    template <class Operation>
    int submit(Operation& op) noexcept;          // prepare + submit

    template <class Function>
    void submit_batch(Function&& fn) noexcept;   // batch prepare+submit

    // Locked submission (caller holds the uring gate)
    template <class Operation>
    int prepare_locked(Operation& op) noexcept;
    int submit_locked() noexcept;

    // uring gate acquisition
    [[nodiscard]] uring_lock lock_uring() const noexcept;
    [[nodiscard]] uring_lock try_lock_uring() const noexcept;

    // Sender factories
    [[nodiscard]] auto async_poll(descriptor_view descriptor,
                                   unsigned poll_mask);
    [[nodiscard]] auto async_resolve(dns_query query,
                                      dns_result_view result);
    [[nodiscard]] auto async_resolve(std::string_view host,
                                      std::string_view service,
                                      dns_result_view result);

    void notify_waiters() noexcept;
    void notify_one_waiter() noexcept;

    int post(io_uring_operation_base& op) noexcept;
    void run() noexcept;
    int stop() noexcept;
    [[nodiscard]] bool is_in_context() const noexcept;
};
```

The `uring_lock` is an RAII guard for exclusive SQ/CQ access. It is acquired
via `lock_uring()` (spinning) or `try_lock_uring()` (non-blocking).

`io_uring_context_options::event_fd` may name a caller-owned eventfd. A
negative value, the default, makes the context create and own a private eventfd.

### `io_uring_context_options`

```cpp
struct io_uring_context_options {
    unsigned entries = 256;
    unsigned setup_flags = IORING_SETUP_COOP_TASKRUN;
    unsigned cqe_batch_window = 64;
    unsigned wait_spin_count = 4;
    unsigned cqe_inline_completion_threshold = 64;
    unsigned local_queue_threshold = 0;
    bool enable_sqpoll = false;
    unsigned sqpoll_thread_cpu = 0;
    unsigned sqpoll_idle_ms = 1000;
    int event_fd = -1;
};
```

All operations derive from `io_uring_operation_base`:

```cpp
class io_uring_operation_base {
public:
    io_uring_operation_base* next = nullptr;
    operation_stack_state* stack_state = nullptr;
    int result = 0;
    unsigned flags = 0;

    // non-copyable, non-movable (intrusive list node)

    virtual ~io_uring_operation_base() = default;
    virtual void execute() noexcept = 0;
};

struct operation_stack_state {
    io_uring_operation_base* head = nullptr;

    void push(io_uring_operation_base& op) noexcept;
    void push(io_uring_operation_base* ops) noexcept;
    [[nodiscard]] io_uring_operation_base* pop_all() noexcept;
    [[nodiscard]] bool empty() const noexcept;
};
```

---
