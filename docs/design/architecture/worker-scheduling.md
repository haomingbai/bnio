# Worker Scheduling: Local-State Registry, Stealing, and Directed Wakeup

This document describes how `bnio` schedules CPU work across the threads that
call `io_context::run()`. It covers the two worker-state lists (run/suspend),
the unified `fetch_cpu_task()` path, work stealing, and the directed (single
worker) wakeup used by the `io_context` publish paths.

The design is identical for both native backends. `kqueue_task_queue_state`
(BSD) and `io_uring_task_queue_state` (Linux) are field-for-field
corresponding; every reference below names the kqueue types for brevity.

## 1. Problem statement

### 1.1 Fairness

A worker that drains the shared CPU queue with one `pop_cpu_all()` takes the
whole queue into its own stack before executing anything. With many workers
this is unfair: the first worker to check hoards all tasks, and every other
worker goes idle. Measurements on 8 workers showed a single worker consuming
100% CPU while the rest sat at 0%.

### 1.2 Thundering herd

The original wake path wrote the shared `wake_channel_` once; every worker
with read interest registered on that fd woke up and raced to pop the queue,
even though only one task was published.

### 1.3 Use-after-free

Workers create their local task state inside the native context they own.
When `run()` returns, the context (and its `local_state_`) is destroyed. A
remote thread that steals work must never touch a `local_state_` after its
owning worker has exited — otherwise it dereferences freed memory.

## 2. Data structures

### 2.1 `local_task_queue_state` — per-worker state

```cpp
struct kqueue_local_task_queue_state {
  void push_cpu(kqueue_operation_base& operation) noexcept;
  [[nodiscard]] kqueue_operation_base* pop_cpu_all() noexcept;
  [[nodiscard]] bool has_cpu_tasks() const noexcept;  // relaxed peek, caller holds the list lock
  std::atomic<kqueue_operation_base*> cpu_head;  // MPSC local CPU queue
  kqueue_local_task_queue_state* prev;            // doubly-linked list links
  kqueue_local_task_queue_state* next;
  bnio::base::wake_channel wake_channel_;         // per-worker directed wake
};
```

The local CPU queue is a lock-free MPSC LIFO (CAS push, `exchange` pop). It is
owned by the worker for pushes and pops, and by a remote stealer for
`pop_cpu_all()` while it holds the list lock. `has_cpu_tasks()` is a relaxed
peek used by the stealing probe before the locked `pop_cpu_all()` exchange,
so a doomed probe costs one relaxed load instead of a locked RMW.

### 2.2 `worker_state_list` — one list + its lock

```cpp
struct kqueue_worker_state_list {
  std::mutex lock;                              // guards the list + node lifetime
  kqueue_local_task_queue_state* head;
  kqueue_local_task_queue_state* tail;          // kept so rotation to the tail is O(1)
  kqueue_local_task_queue_state* cursor;        // round-robin cursor (suspend list only)
};
```

The lock and the list head live in the same struct so that every operation on
the list takes exactly the lock that owns it. There is no way to touch a list
node without holding its lock — that is what makes node lifetime safe.

The `tail` pointer is maintained by the link/unlink helpers and exists so the
stealing probe order can rotate round-robin in O(1) (move the probed head node
to the tail). The `cursor` is used **only** by the suspend list, for the
directed-wake rotation in `wake_one_sleeping()`; the run list achieves its
rotation through `tail` instead, so stealing never reads a cursor.

### 2.3 `worker_state_registry` — the two lists

```cpp
struct kqueue_worker_state_registry {
  kqueue_worker_state_list run;      // workers processing work
  kqueue_worker_state_list suspend;  // workers sleeping in the native poller
};
```

A worker is linked into **exactly one** list at any time:

| List    | Contains            | Accessed by                          |
|---------|---------------------|--------------------------------------|
| `run`   | active workers      | stealing (only running workers hold stealable CPU work) |
| `suspend` | sleeping workers   | directed wakeup (`wake_one_sleeping`) |

The `run` list also keeps workers alive for the whole time they could be
touched by a stealer. The `suspend` list keeps a sleeping worker alive while
a publisher wakes it.

### 2.4 The shared state

`kqueue_task_queue_state` owns the registry plus the existing shared queues:

```cpp
struct kqueue_task_queue_state {
  std::atomic<kqueue_operation_base*> cpu_head;      // shared MPSC CPU queue
  std::atomic<kqueue_io_operation_base*> io_head;    // shared MPSC I/O queue
  std::atomic<std::size_t> awake_workers;            // workers not blocked in the poller
  std::atomic<std::size_t> running_workers;          // total workers inside io_context::run() (active + suspended + entering)
  kqueue_worker_state_registry workers;              // run + suspend lists
  std::atomic<int> life_state;                       // 0 = running, 1 = stopping
  bnio::base::wake_channel wake_channel_;            // shared broadcast (stop + native notify_one_waiter)
  std::mutex submit_lock;                            // publish/stop serialization
};
```

`running_workers` counts every thread currently inside `io_context::run()` —
whether it is actively processing work, suspended in the poller, or still
entering the run loop. It is incremented by `io_context::run()` **before any
check** and decremented on every run() return path (`release_worker_slot()`);
the native backends never touch it directly. The early increment closes the
use-after-free window in `stop()`: a worker that has entered `run()` but not
yet `enter_run()` is still counted, so `stop_internal()` waits for it. The
count feeds two heuristics: the steal gate (`2*awake_workers >
running_workers`, §4) and the wake fast path (`awake_workers >=
running_workers`, §6). Both are racy, relaxed advisory checks — because the
count may transiently include workers that have not yet joined the awake set,
they only err on the conservative side.

## 3. The fetch path: `fetch_cpu_task()`

Every worker obtains CPU work through one entry point, tried in order:

```
local queue  →  shared CPU queue  →  steal from another worker
```

```cpp
kqueue_operation_base* kqueue_context::fetch_cpu_task(
    bool allow_steal) noexcept {
  // 1. Worker-local queue first: fastest and preserves locality.
  if (kqueue_operation_base* operations = local_state_.pop_cpu_all())
    return reverse_tasks(operations);

  // 2. Shared CPU queue.
  if (kqueue_operation_base* operations = global_state_->pop_cpu_all())
    return reverse_tasks(operations);

  // 3. Steal from another worker's local queue. Only reached when both
  //    local and shared queues are empty, so a stealing worker is by
  //    definition relatively idle. Suppressed when allow_steal is false
  //    (e.g. the re-checks after begin_wait), since the worker already
  //    tried a steal while running and another one would only pay the
  //    run list's lock for nothing.
  if (!allow_steal) return nullptr;
  return steal_cpu_tasks();
}
```

Each level takes the whole batch at once (`pop_cpu_all()` + `reverse_tasks()`
to restore FIFO order), and the run loop stops at the first level that yields
a batch. Because a batch is fetched and executed before the loop checks
again, work never piles up on one thread's stack; a worker that is truly
idle (both queues empty) is the one that reaches the steal stage.

`allow_steal` defaults to `true`. It is passed `false` on the re-checks that
run **after** `begin_wait()` (§10): the worker already attempted a steal while
running, so a second attempt while marked sleeping would only acquire the run
list's lock to find nothing.

`run_cpu_batch()` is the run-loop wrapper: fetch one batch, execute it, return
whether anything ran.

```cpp
bool kqueue_context::run_cpu_batch(bool allow_steal) noexcept {
  if (kqueue_operation_base* operations = fetch_cpu_task(allow_steal)) {
    execute_tasks(operations);
    return true;
  }
  return false;
}
```

## 4. Work stealing

`steal_cpu_tasks()` runs only when local and shared queues are empty, so the
stealing worker is by definition relatively idle.

```cpp
kqueue_operation_base* kqueue_context::steal_cpu_tasks() noexcept {
  if (global_state_ == nullptr) return nullptr;

  // Conservative gate: only attempt a steal while MORE workers are active
  // than suspended (active > suspend  <=>  2*active > running). A worker
  // suspends only after finding no work, so when the majority are suspended
  // the remaining active workers' local queues are likely empty too, and
  // scanning the run list would just pay run.lock contention for nothing.
  // The two loads are deliberately not coordinated — this is a racy,
  // relaxed heuristic that only decides whether the scan is worth the lock;
  // the actual steal below stays correct regardless of the gate's accuracy.
  // running_workers is the run()-level counter (incremented before
  // enter_run()), so it may transiently exceed active + suspended; the gate
  // then errs on the conservative side, which is fine.
  const std::size_t active =
      global_state_->awake_workers.load(std::memory_order_relaxed);
  const std::size_t running =
      global_state_->running_workers.load(std::memory_order_relaxed);
  if (active * 2 <= running) return nullptr;

  if (!options_.enable_steal) return nullptr;

  // Single-probe steal: only inspect the run-list head (one peer).  The
  // lock guards both the probe and the lifetime of the target node — a
  // worker unregisters under the same lock before its context is destroyed,
  // so the probed node is never freed while we touch it (UAF protection).
  kqueue_worker_state_list& run = global_state_->workers.run;
  std::lock_guard<std::mutex> guard(run.lock);

  kqueue_local_task_queue_state* const target = run.head;
  if (target == nullptr || target == &local_state_) return nullptr;

  if (target->has_cpu_tasks()) {
    if (kqueue_operation_base* operations = target->pop_cpu_all()) {
      // Rotate the victim to the tail so the head advances for the next
      // stealer — all workers share one global probe order.
      kqueue_rotate_local_state_to_tail(run, target);
      return reverse_tasks(operations);
    }
  }

  // Head was empty — rotate it to the tail anyway so the next stealer
  // probes a different node instead of hitting the same empty one.
  kqueue_rotate_local_state_to_tail(run, target);
  return nullptr;
}
```

Key points:

- **A conservative gate runs before the lock.** `2*awake_workers >
  running_workers` is a racy, relaxed heuristic: a worker suspends only after
  finding no work, so when the majority of workers are suspended the remaining
  active workers' local queues are likely empty too, and taking `run.lock`
  would be wasted contention. The two counter loads are deliberately not
  synchronized; the steal below remains correct regardless of the gate's
  accuracy. `running_workers` is maintained by `io_context::run()` and may
  transiently exceed active + suspended, which only makes the gate more
  conservative.
- **`enable_steal` makes stealing optional.** `kqueue_context_options` /
  `io_uring_context_options` expose a boolean `enable_steal` (default `true`).
  When `false`, `steal_cpu_tasks()` returns `nullptr` right after the gate,
  skipping `run.lock` and its cache-line traffic entirely. The shared MPSC
  queue already distributes work fairly for most workloads; disabling steal is
  useful when the tail-balancing benefit does not justify the lock cost.
- **Single-probe, not a full scan.** Only the run-list head is inspected. The
  lock guards both the probe and the target node's lifetime — a worker
  unregisters under the same lock before its context is destroyed, so the
  probed node is never freed while we touch it (UAF protection).
- **Shared round-robin via list rotation.** Whether the head had tasks or not,
  it is rotated to the tail (`kqueue_rotate_local_state_to_tail`, O(1) thanks
  to the maintained `tail` pointer). All workers therefore share one global
  probe order instead of each keeping a private cursor, and a repeated probe
  never hits the same empty node twice.
- **Bulk steal, stop at first hit.** The whole victim queue is taken in one
  `pop_cpu_all()`, and traversal stops after the single probe. This matches
  the "batch, not single-task" policy used by `fetch_cpu_task`.

## 5. Sleep/wake transitions

### 5.1 `begin_wait()` — enter the suspend list

When a worker decides to block in the native poller, it moves itself from the
run list to the suspend list **before** calling the blocking syscall, so a
publisher can find it there.

```cpp
void kqueue_context::begin_wait() noexcept {
  run_state_.waiting.store(true, std::memory_order_release);
  if (global_state_ == nullptr) return;
  {
    std::lock_guard<std::mutex> guard(global_state_->workers.run.lock);
    kqueue_unlink_local_state(global_state_->workers.run, &local_state_);
  }
  {
    std::lock_guard<std::mutex> guard(global_state_->workers.suspend.lock);
    kqueue_link_local_state(global_state_->workers.suspend, &local_state_);
  }
  global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
}
```

### 5.2 `end_wait()` — re-enter the run list

Waking up is the mirror image; the worker rejoins the run list **before**
processing any work, so a concurrent stealer can see it again.

```cpp
void kqueue_context::end_wait() noexcept {
  if (global_state_ != nullptr) {
    {
      std::lock_guard<std::mutex> guard(global_state_->workers.suspend.lock);
      kqueue_unlink_local_state(global_state_->workers.suspend, &local_state_);
    }
    {
      std::lock_guard<std::mutex> guard(global_state_->workers.run.lock);
      kqueue_link_local_state(global_state_->workers.run, &local_state_);
    }
    global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  }
  run_state_.waiting.store(false, std::memory_order_release);
}
```

### 5.3 Lock ordering

The two list locks are **never nested**. `begin_wait()` takes run lock,
releases it, then takes suspend lock. `end_wait()` does the reverse order.
Because a lock is released before the next is taken, a waking worker and a
sleeping worker can never deadlock against each other. The only place that
touches both lists is `unregister_local_state()` at shutdown, and it also
takes each lock separately.

The `submit_lock` (held by publishers and `stop()`) is acquired before
`suspend` lock in `wake_one_sleeping_locked()`; no path takes `submit_lock`
while holding a list lock, so there is no lock-ordering cycle.

## 6. Directed wakeup: `wake_one_sleeping()`

The shared `wake_channel_` is the broadcast path used by `stop()` (which must
wake every worker) and as a fallback when nobody is suspended. It is also
written by the native contexts' own cross-thread publication path:
`kqueue_context::notify_one_waiter()` / `io_uring_context::signal_eventfd()`
(`post()`/`publish_io()` from a foreign thread) write the shared channel
directly, waking every suspended worker rather than just one. Directed wakeup
targets the `io_context`-level publish paths (`publish_cpu()`, `publish_io()`,
timer wakeups), where exactly one sleeping worker is enough. Each worker
therefore listens to **two** wake sources:

1. its **per-worker** `wake_channel_` (directed wake);
2. the **shared** `wake_channel_` (broadcast wake — stop, fallback, and native
   `notify_one_waiter`).

On kqueue both fds are registered with `EVFILT_READ | EV_CLEAR` in
`enter_run()` and distinguished by their `udata` sentinel in
`process_event()`. On io_uring both fds get an `IORING_POLL_ADD` SQE with
separate user-data sentinels.

```cpp
bool kqueue_task_queue_state::wake_one_sleeping() noexcept {
  kqueue_worker_state_list& suspend = workers.suspend;
  std::lock_guard<std::mutex> guard(suspend.lock);

  // Validate the cursor is still in the list, else restart from the head.
  kqueue_local_task_queue_state* start = suspend.head;
  if (suspend.cursor != nullptr) {
    kqueue_local_task_queue_state* scan = start;
    while (scan != nullptr && scan != suspend.cursor) scan = scan->next;
    if (scan != nullptr) start = suspend.cursor;
    else suspend.cursor = nullptr;
  }
  if (start == nullptr) return false;

  (void)start->wake_channel_.wake();   // directed: only this worker wakes
  suspend.cursor = start->next;        // rotate for fairness
  return true;
}
```

`io_context::wake_one_sleeping_locked()` (called from `publish_cpu()`,
`publish_io()`, and timer wakeups) prefers the directed wake and falls back to
the shared broadcast channel only when nobody is suspended:

```cpp
void io_context::wake_one_sleeping_locked() noexcept {
  if (global_state_.life_state.load(std::memory_order_acquire) != 0) return;

  // Fast path: when every worker that entered run() is still awake
  // (awake_workers >= running_workers), nobody is suspended and
  // there is no worker to wake.  The two acquire loads are not
  // synchronised with each other — this is a racy advisory check
  // that only decides whether to skip the expensive suspend.lock
  // acquisition and the fallback broadcast write.  running_workers
  // is the run()-level counter incremented before enter_run(), so it
  // may transiently include workers that have not yet joined the
  // awake set — that only makes the fast path skip more often
  // (harmless, same cost as before).  A false negative (a worker
  // just entered begin_wait but awake_workers has not yet been
  // decremented) merely delays the wake by one run-loop iteration,
  // which is harmless; a false positive falls through to the
  // existing path and finds nobody to wake — same cost as before.
  if (global_state_.awake_workers.load(std::memory_order_acquire) >=
      global_state_.running_workers.load(std::memory_order_acquire)) {
    return;
  }

  if (!global_state_.wake_one_sleeping()) {
    wake_locked();   // nobody suspended → broadcast
  }
}
```

The **fast path** is a racy advisory check: when `awake_workers >=
running_workers`, every worker that entered `run()` is still awake, so nobody
is suspended and there is no worker to wake. It exists to skip the `suspend.lock`
acquisition and the fallback broadcast write on the hot publish path. A false
negative (a worker entered `begin_wait()` but has not yet decremented
`awake_workers`) merely delays the wake by one run-loop iteration, which is
harmless; a false positive falls through to the existing path and finds nobody
to wake, costing the same as before. Because `running_workers` is the
`run()`-level counter (incremented before `enter_run()`), it may transiently
include workers that have not yet joined the awake set — the check then skips
less often, never incorrectly.

The directed write is safe against the worker's destruction because the
worker unregisters under the suspend list lock before its context is
destroyed: `wake_one_sleeping()` holds that lock for the whole write, so it
can never write a channel that is about to be closed.

### 6.1 Timer wakeups: `wake_one_if_all_workers_sleeping()`

Timer deadlines are delivered by each worker's blocking timeout: a worker
blocked in `io_uring_enter` / `kevent()` with a timeout wakes itself when its
deadline arrives. So a new or earlier timer deadline only needs to wake a
worker when **every** worker is already blocked (their armed timeouts would
fire too late); otherwise the awake worker observes the new nearest deadline on
its next run-loop pass and re-arms its own timeout.

```cpp
void io_context::wake_one_if_all_workers_sleeping() noexcept {
  std::lock_guard<std::mutex> guard(global_state_.submit_lock);
  // Reaching here with all workers sleeping implies a genuine re-arm is
  // needed (the heap deadline moved earlier, or a wait was canceled).
  if (global_state_.awake_workers.load(std::memory_order_acquire) != 0) {
    return;
  }
  wake_one_sleeping_locked();
}
```

Waking exactly one sleeping worker is sufficient: it re-arms at the earlier
deadline and dispatches it when it fires. The timer entry points stage this
wake only when the heap deadline moved earlier (or a wait was canceled), so
reaching the function with all workers sleeping implies a genuine re-arm is
needed.

## 7. Lifecycle: register / unregister

```cpp
void kqueue_context::register_local_state() noexcept {
  kqueue_worker_state_list& run = global_state_->workers.run;
  std::lock_guard<std::mutex> guard(run.lock);
  kqueue_link_local_state(run, &local_state_);   // head insertion
}

void kqueue_context::unregister_local_state() noexcept {
  // The worker is in exactly one list; unlink from whichever it is in.
  {
    std::lock_guard<std::mutex> guard(global_state_->workers.run.lock);
    if (kqueue_local_state_in_list(global_state_->workers.run, &local_state_)) {
      kqueue_unlink_local_state(global_state_->workers.run, &local_state_);
      return;
    }
  }
  {
    std::lock_guard<std::mutex> guard(global_state_->workers.suspend.lock);
    if (kqueue_local_state_in_list(global_state_->workers.suspend, &local_state_)) {
      kqueue_unlink_local_state(global_state_->workers.suspend, &local_state_);
    }
  }
}
```

`register_local_state()` runs in `enter_run()` after the wake fds are
registered; `unregister_local_state()` runs after the run loop exits, **before**
the native context (and its `local_state_`) is destroyed. This ordering is the
foundation of the UAF guarantee: any thread holding a list lock is either
before unregister (node alive) or after it (node already unlinked and no
longer reachable).

## 8. I/O handling

Local I/O queues were removed. All I/O publications contend on the shared I/O
queue (`global_state_->push_io()`), and `consume_io_tasks()` takes the whole
I/O batch at once. Batch unfairness for I/O is accepted: io_uring and kqueue
are batch-capable, and a worker that cannot handle a batch simply leaves it
for the next fetch pass. There is no I/O stealing.

Standalone kqueue contexts (no `global_state_`) keep a private `local_io_head_`
as a fallback drained by `consume_io_tasks()`; this path is not used by
`io_context`. The io_uring backend has no such fallback — its
`consume_io_tasks()` returns immediately when `global_state_` is null.

## 9. Why the registry structure is shaped this way

- **Lock and list head in one struct.** Every traversal is
  `lock(list) { walk list }` — there is no invariant that depends on which
  other list the same node might also be in.
- **Two lists instead of one.** A single list would force either stealing or
  waking to walk sleeping workers too. Splitting them means stealing needs the
  run lock only, and waking needs the suspend lock only — the two operations
  never contend with each other.
- **Doubly-linked with per-list locks.** Unlinking a node is O(1) given the
  `prev` link, which matters for `begin_wait()`/`end_wait()`/unregister on a
  per-publish or per-wake path.
- **Round-robin fairness, cursor for waking and rotation for stealing.** The
  suspend list keeps a `cursor` so `wake_one_sleeping()` resumes after the
  last woken worker. The run list keeps a `tail` instead and rotates the
  probed node to the tail, so all workers share one global steal probe order.
  Head insertion never invalidates either mechanism (existing node addresses
  do not move), and the cursor's validation scan handles the only
  invalidation case — the cursor's node being unregistered.

## 10. Run-loop integration

The run-loop phase machine uses `run_cpu_batch()` as its single CPU-work
entry:

```
handle_run_ready_tasks:
    collect ready native events (non-blocking)
    run_cpu_batch()                    → local → shared → steal, one batch
    consume_timeout_operations()
    consume_io_tasks()
    wait for work (spin, then block)
```

The blocking wait path is:

```
wait_for_io_work:
    begin_wait()                       → move run → suspend
    recheck events / cpu / timer / io  → end_wait() + rerun if any work
    compute deadline from timer heap
    block in native poller (both wake fds registered)
    end_wait()                         → move suspend → run
    recheck cpu / timer / io           → run_ready_tasks
```

`begin_wait()` is called **before** the recheck so a publisher that lands
during the recheck window finds the worker in the suspend list and can wake
it; `end_wait()` restores the worker to the run list before it processes any
work.

The post-`begin_wait()` rechecks pass `allow_steal = false` to
`run_cpu_batch()` (as does the timer recheck inside
`compute_io_wait_timeout()`): the worker already attempted a steal while it
was running, so a second attempt while marked sleeping would only take the
run list's lock to find nothing — `begin_wait()` has already placed the worker
on the suspend list, where a stealer cannot reach its (empty) local queue
anyway.

### 10.1 Wake-poll re-arm policy and the no-unbounded-block invariant

Both wake polls are re-armed immediately before the blocking native wait, and
the armed/not-armed distinction is explicit. On io_uring,
`submit_eventfd_poll()` / `submit_local_eventfd_poll()` return 1 when the poll
is armed (newly submitted or already pending), 0 when it is **not** armed
because the context is stopping, and a negative errno on submission failure. A
`0` tells the caller the ring must not be entered unless another wake source
exists.

Re-arm failures are classified at the single policy point in
`wait_for_io_work()` (a re-arm failure while collecting an eventfd CQE is
deliberately not terminal there — it is re-handled by that same point, so
transient pressure never half-closes the context):

- `-EAGAIN` is transient SQ pressure (typically SQPOLL, where only the kernel
  poll thread frees SQ slots): the poll is not armed, so the worker never
  blocks — it returns to the ready-tasks phase and a later pass re-arms
  successfully.
- Any other negative return is a fatal re-arm failure (e.g. a closed wake
  channel) and routes through the finish drain, which aborts and delivers
  every operation before the loop exits.
- A `0` (poll skipped because the context is stopping) permits blocking only
  under graceful-stop semantics — inflight kernel operations whose CQEs will
  wake the worker, or a bounded timeout. `should_finish()` is re-evaluated
  after the re-arms so a completion racing the stop transition can never leave
  the worker parked without a wake source.

Together these guarantee the invariant that the blocking native wait
(`io_uring_enter` / `kevent()`) is never entered unbounded without a wake
source: an armed eventfd poll, a bounded timeout, or inflight kernel
operations being grace-waited.
