# io_uring Setup Optimizations

This page documents the io_uring setup flags and runtime optimizations
applied by `bupp::async_io::linux_native::io_uring_context`.

## Default Setup Flags

Since kernel 5.19 (2022), Linux supports two flags that materially reduce
per-submission overhead when the application follows a single-threaded event
loop model — exactly the model that `io_uring_context::run()` implements.

### `IORING_SETUP_SINGLE_ISSUER`

| Property | Value |
|---|---|
| Kernel requirement | Linux ≥ 6.0 |
| What it does | Tells the kernel that only one thread will submit SQEs. The kernel skips internal `mutex_lock`/`mutex_unlock` pairs around every submission. |
| Why it helps | Eliminates kernel-side locking on the hot `io_uring_enter` path. |
| Compatibility fallback | If `io_uring_queue_init_params` returns `-EINVAL`, the library retries without this flag. |

### `IORING_SETUP_COOP_TASKRUN`

| Property | Value |
|---|---|
| Kernel requirement | Linux ≥ 5.19 |
| What it does | Lets the kernel defer task_work execution until the application explicitly enters the kernel to wait for CQEs. Fewer involuntary context switches. |
| Why it helps | Reduces redundant `io_uring_enter` calls and keeps the CPU on the application's run loop longer. |
| Compatibility fallback | Same retry-on-EINVAL strategy. |

### Default Value

`io_uring_context_options::setup_flags` defaults to:

```cpp
IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN
```

Applications that need the old behaviour can explicitly set `setup_flags = 0`.

## Runtime: Conditional Mutex Bypass

When `IORING_SETUP_SINGLE_ISSUER` is accepted by the kernel, `io_uring_context`
sets an internal flag `single_issuer_ = true`. Two hot paths then skip
`uring_mutex_`:

### CQE Collection (`collect_cqe_tasks`)

```
spin_for_work() → collect_ready_cqes() → collect_cqe_tasks()
                                                    ↑
                                          skips uring_mutex_ when
                                          single_issuer_ == true
```

`collect_cqe_tasks()` reads CQEs via `ring_.consume_ready_cqes()`.  With
`SINGLE_ISSUER` the kernel guarantees no concurrent submission, so reading
CQEs without a userspace lock is safe.

### Submission (`submit`, `submit_wake_task`)

`submit()` and `submit_wake_task()` acquire `uring_mutex_` only when
`single_issuer_` is false.

## Kernel Feature Probing

`queue_init()` now calls `ring_.queue_init_params()` instead of
`ring_.queue_init()`.  After a successful init, `params.features()` is
available for future use (e.g. detecting `IORING_FEAT_NODROP`,
`IORING_FEAT_FAST_POLL`).

## SQPOLL Option

`io_uring_context_options` exposes three new fields:

```cpp
struct io_uring_context_options {
  // ... existing fields ...

  /// When true, adds IORING_SETUP_SQPOLL to setup_flags.
  /// A kernel thread polls the SQ ring, eliminating io_uring_enter syscalls
  /// for submission.  Costs one dedicated CPU core.
  bool enable_sqpoll = false;

  /// CPU affinity hint for the SQPOLL kernel thread (0 = no preference).
  unsigned sqpoll_thread_cpu = 0;

  /// SQPOLL idle timeout in milliseconds before the kernel thread parks.
  unsigned sqpoll_idle_ms = 1000;
};
```

SQPOLL is opt-in because it burns a CPU core permanently.  It is most
beneficial for dedicated server processes where submission latency matters
more than CPU efficiency.

## Backward Compatibility

On kernels older than 5.19, `io_uring_queue_init_params` returns `-EINVAL`
for the new flags.  The library detects this and retries with `setup_flags
& ~(IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN)`, falling back
to the pre-optimization behaviour transparently.
