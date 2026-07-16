# io_uring Setup Optimizations

This page documents the io_uring setup flags applied by
`bupp::async_io::linux_native::io_uring_context`.

## Default Setup Flags

Since kernel 5.19 (2022), Linux supports `IORING_SETUP_COOP_TASKRUN` and
`IORING_SETUP_SINGLE_ISSUER`. The former reduces task-work wakeups; the latter
lets each single-thread-submitted ring avoid kernel-side submission locking.

### `IORING_SETUP_COOP_TASKRUN`

| Property | Value |
|---|---|
| Kernel requirement | Linux ≥ 5.19 |
| What it does | Lets the kernel defer task_work execution until the application explicitly enters the kernel to wait for CQEs. Fewer involuntary context switches. |
| Why it helps | Reduces redundant `io_uring_enter` calls and keeps the CPU on the application's run loop longer. |
| Compatibility fallback | Same retry-on-EINVAL strategy. |

### `IORING_SETUP_SINGLE_ISSUER`

| Property | Value |
|---|---|
| Kernel requirement | Linux ≥ 5.19 |
| What it does | Declares that only one task submits requests to the ring. |
| Why it helps | Removes submission-side kernel locking that is unnecessary for bupp's one-thread-one-ring model. |
| Compatibility fallback | Same retry-on-EINVAL strategy. |

### Default Value

`io_uring_context_options::setup_flags` defaults to:

```cpp
IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER
```

Applications that need the old behaviour can explicitly set `setup_flags = 0`.

## Kernel Feature Probing

`queue_init()` now calls `ring_.queue_init_params()` instead of
`ring_.queue_init()`.  After a successful init, `params.features()` is
available for future use (e.g. detecting `IORING_FEAT_NODROP`,
`IORING_FEAT_FAST_POLL`).

## SQPOLL Option

`io_uring_context_options` exposes SQPOLL fields alongside other tuning knobs:

```cpp
struct io_uring_context_options {
  unsigned entries = 256;
  unsigned setup_flags =
      IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;
  unsigned cqe_batch_window = 64;
  unsigned wait_spin_count = 4;
  unsigned cqe_inline_completion_threshold = 64;
  unsigned local_queue_threshold = 0;

  /// When true, adds IORING_SETUP_SQPOLL to setup_flags.
  /// A kernel thread polls the SQ ring, eliminating io_uring_enter syscalls
  /// for submission.  Costs one dedicated CPU core.
  bool enable_sqpoll = false;

  /// CPU affinity hint for the SQPOLL kernel thread (0 = no preference).
  unsigned sqpoll_thread_cpu = 0;

  /// SQPOLL idle timeout in milliseconds before the kernel thread parks.
  unsigned sqpoll_idle_ms = 1000;

  /// Optional non-owning eventfd used to wake the context run loop.
  int event_fd = -1;
};
```

SQPOLL is opt-in because it burns a CPU core permanently.  It is most
beneficial for dedicated server processes where submission latency matters
more than CPU efficiency.

## Backward Compatibility

On kernels older than 5.19, `io_uring_queue_init_params` returns `-EINVAL`
for the new flags. The library detects this and retries after removing
`IORING_SETUP_COOP_TASKRUN`, `IORING_SETUP_SINGLE_ISSUER`, and any enabled
`IORING_SETUP_SQPOLL`, falling back transparently.
