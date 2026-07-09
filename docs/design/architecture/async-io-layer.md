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
and provides an event loop with intrusive operation scheduling. Cross-thread
producers publish work through an MPSC intrusive stack and wake the single
consumer with an eventfd-backed poll request; this low-level loop does not use
`std::mutex` or `std::condition_variable`.

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

`io_uring_context_options::event_fd` may name a caller-owned eventfd. A
negative value, the default, makes the context create and own a private eventfd.

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
