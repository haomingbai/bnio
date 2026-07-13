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
`io_context`'s native worker pool). All workers in one high-level context share
an `io_uring_task_queue_state`. That state contains separate MPSC CPU and I/O
queues, the number of workers currently published as awake, and the closing
state of the whole worker group. It deliberately has no I/O count, batch
threshold, explicit-drain flag, timer, or lock. A
standalone `io_uring_context` uses its non-atomic local CPU/I/O queues instead
of creating a private object that pretends to be global state.

```cpp
class io_uring_context {
public:
    io_uring_context() noexcept;
    explicit io_uring_context(const io_uring_context_options& opts) noexcept;
    ~io_uring_context() noexcept;

    // non-copyable, non-movable (owns a ring and run-loop state)

    int queue_init(const io_uring_context_options& opts) noexcept;
    void queue_exit() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] unsigned kernel_features() const noexcept;
    void set_global_state(io_uring_task_queue_state* state) noexcept;

    // Sender factories
    [[nodiscard]] auto async_poll(descriptor_view descriptor,
                                   unsigned poll_mask);
    [[nodiscard]] auto async_resolve(dns_query query,
                                      dns_result_view result);
    [[nodiscard]] auto async_resolve(std::string_view host,
                                      std::string_view service,
                                      dns_result_view result);

    void notify_one_waiter() noexcept;
    [[nodiscard]] bool is_waiting() const noexcept;

    int post(io_uring_operation_base& op) noexcept;
    void publish_io(io_uring_io_operation_base& op) noexcept;
    void run() noexcept;
    int stop() noexcept;
    [[nodiscard]] bool is_in_context() const noexcept;
};
```

Each context stores one normalized `io_uring_context_options` value instead of
copying its fields into separate members. Its single run thread is the only
owner of SQ preparation, submission, and CQ collection, so the Linux async-io
layer contains no ring mutex. Low-level senders publish an
`io_uring_io_operation_base` to the context's selected I/O queue. There is no
public raw prepare, submit, or batch-submit interface. Only the run loop takes
I/O operations, fills SQEs, and calls the private ring-submission helper.
Ring-affine internal control I/O created by the owning run thread uses a local
I/O chain. Ordinary sender I/O enters the shared I/O queue when
`global_state_` is set, and otherwise enters the context's local I/O chain.

The run loop always checks CPU work first. It then consumes ring-local control
I/O followed by every operation atomically taken from the shared I/O queue.
Before eventfd wait it publishes the local waiting flag, decrements the shared
awake count, and repeats the CQE/CPU/I/O checks. A producer publishing after
that transition writes eventfd to wake one worker. This supplies low-load
progress while busy workloads form batches naturally.

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

`set_global_state()` selects the execution model before work starts. Its
default null pointer keeps all work in the native context's single-threaded
local CPU/I/O queues. A non-null pointer selects externally owned shared state
whose owner must keep it alive until the context stops. `io_context` owns one
such state and calls the setter for every native worker. `io_uring_context`
never owns or manufactures a fallback global state.

CPU operations derive from `io_uring_operation_base`. I/O operations add the
prepare contract and a separate intrusive link:

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

class io_uring_io_operation_base : public io_uring_operation_base {
public:
    io_uring_io_operation_base* io_next = nullptr;
    virtual void prepare(base::submission_queue_entry& sqe) noexcept = 0;
    virtual void complete_submit_error(int result) noexcept = 0;
    virtual bool ring_affine() const noexcept; // false for ordinary I/O
};

struct io_uring_task_queue_state {
    std::atomic<io_uring_operation_base*> cpu_head;
    std::atomic<io_uring_io_operation_base*> io_head;
    std::atomic<std::size_t> awake_workers;
    std::atomic_bool closing;
};
```

`closing` belongs to the worker group. Once `io_context::stop()` sets it, every
worker treats the run loop as finishing, including a worker racing with late
registration.

CPU work always has priority. The run loop drains its local CPU list and the
shared CPU queue before atomically taking every operation currently published
to the shared I/O queue. Ring-local control I/O is consumed before shared I/O,
but only after CPU work is exhausted.

Parking is a two-stage publication. The worker first marks its local state as
waiting, then decrements `awake_workers`. It rechecks CQEs and CPU work and
again takes all I/O before reading eventfd. If work appeared, it
increments `awake_workers`, reopens locally, and resumes. A producer that sees
a waiting worker writes that worker's eventfd, closing the lost-wakeup window
without a timer.

---
