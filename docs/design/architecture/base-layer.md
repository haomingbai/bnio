# Layer 1: `bupp::base` — Thin System Call Wrappers

Namespace `bupp::base`. Linux headers in `include/bupp/base/linux/`, BSD
headers in `include/bupp/base/bsd/`.

On Linux, maps the `liburing` C API to C++ objects with RAII where appropriate.
Method names drop the `io_uring_` prefix (e.g. `io_uring_submit` →
`ring::submit()`).

On BSD, maps the `kqueue`/`kevent` C API to C++ objects following the same
pattern: `base::kqueue` owns the kqueue fd, and `base::event` wraps `struct
kevent`.

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

### `kqueue` — kqueue Descriptor Owner

RAII wrapper around a native kqueue fd. **The sole owner of the kqueue
instance.** Available under `bupp::base` on BSD platforms.

```cpp
class kqueue {
public:
    kqueue() noexcept;
    ~kqueue() noexcept;                   // calls close()

    kqueue(const kqueue&) = delete;
    kqueue& operator=(const kqueue&) = delete;
    kqueue(kqueue&& other) noexcept;
    kqueue& operator=(kqueue&& other) noexcept;

    int open() noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] int native_fd() const noexcept;

    int control(const event* changelist, int nchanges,
                event* eventlist, int nevents,
                const timespec* timeout) noexcept;
};
```

### `event` — `struct kevent` Wrapper

Wraps `struct kevent` construction and field access. Non-owning by default;
caller manages the underlying storage.

### `event_list_view` — Non-owning kevent Array View

Wraps a caller-owned array of `struct kevent` for use with
`kqueue::control()`.

### Design Rules

Per [`maintaince.md`](../../maintaince.md):

- Return semantics mirror the underlying system calls: ≥ 0 on success, negative
  `errno` on failure.
- No exceptions thrown.
- No executors, coroutines, schedulers, or higher-level async models.
- Base layer does **not** own fd, buffer, address, path, or message lifetimes
  unless a type explicitly documents ownership.

---
