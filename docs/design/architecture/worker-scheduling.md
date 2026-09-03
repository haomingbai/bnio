# Worker Scheduling: Suspend-List Registry and Directed Wakeup

This document describes how `bnio` schedules CPU work across the threads that
call `io_context::run()`. It covers the worker suspend list, the unified
`fetch_cpu_task()` path, and the directed (single worker) wakeup used by the
`io_context` publish paths.

Work stealing is intentionally not part of the design: the shared MPSC CPU
queue already distributes work fairly, and a steal probe would add a global
lock and cache-line traffic to every idle transition. Removing it lets the
worker-local queue drop its atomics entirely (see §2.1).

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
publisher that wakes a worker must never touch a `local_state_` after its
owning worker has exited — otherwise it dereferences freed memory.

## 2. Data structures

### 2.1 `local_task_queue_state` — per-worker state

```cpp
struct kqueue_local_task_queue_state {
  void push_cpu(kqueue_operation_base& operation) noexcept;
  [[nodiscard]] kqueue_operation_base* pop_cpu_all() noexcept;
  kqueue_operation_base* cpu_head;                 // plain pointer, not atomic

  void push_io(kqueue_io_operation_base& operation) noexcept;
  [[nodiscard]] kqueue_io_operation_base* pop_io_all() noexcept;
  kqueue_io_operation_base* io_head;               // plain pointer, not atomic

  kqueue_local_task_queue_state* prev;            // doubly-linked list links
  kqueue_local_task_queue_state* next;
  bnio::base::wake_channel wake_channel_;         // per-worker directed wake
};
```

The local CPU queue is a plain LIFO head pointer: it is pushed only by the
worker thread itself (`post()` with `current_context_ == this`, timer and I/O
completion staging inside the run loop) and popped only by the same worker
during fetch. No remote thread ever touches it, so push is a plain head
insert and `pop_cpu_all()` is a plain read-and-clear — no CAS, no `exchange`,
no acquire/release fences on the hot path.

The local I/O queue is the same kind of plain LIFO head pointer (see §7):
`publish_io()` pushes to it only when the caller is this worker's own run-loop
thread, and `consume_io_tasks()` is its only popper. That is what makes it
safe without a lock, an atomic, or a wakeup — the publisher is the thread that
drains the queue — and what keeps an operation on the worker that owns the
connection. The two backends link it through different fields: kqueue uses
`io_next`, io_uring uses the `next` field inherited from
`io_uring_operation_base`, because on io_uring `io_next`/`io_prev` belong
exclusively to the inflight doubly-linked list.

### 2.2 `worker_state_list` — the suspend list

```cpp
struct kqueue_worker_state_list {
  std::mutex lock;                              // guards the list + node lifetime
  kqueue_local_task_queue_state* head;
  kqueue_local_task_queue_state* cursor;        // round-robin cursor for wake_one_sleeping()
};
```

The lock and the list head live in the same struct so that every operation on
the list takes exactly the lock that owns it. There is no way to touch a list
node without holding its lock — that is what makes node lifetime safe.

### 2.3 The shared state

`kqueue_task_queue_state` owns the suspend list plus the existing shared
queues:

```cpp
struct kqueue_task_queue_state {
  std::atomic<kqueue_operation_base*> cpu_head;      // shared MPSC CPU queue
  std::atomic<kqueue_io_operation_base*> io_head;    // shared MPSC I/O queue
  std::atomic<std::size_t> awake_workers;            // workers not blocked in the poller
  std::atomic<std::size_t> running_workers;          // total workers inside io_context::run() (active + suspended + entering)
  kqueue_worker_state_list workers;                  // sleeping workers (directed-wake targets)
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
count feeds the wake fast path (`awake_workers >= running_workers`, §5).

## 3. The fetch path: `fetch_cpu_task()`

Every worker obtains CPU work through one entry point, tried in order:

```
local queue  →  shared CPU queue
```

```cpp
kqueue_operation_base* kqueue_context::fetch_cpu_task() noexcept {
  // 1. Worker-local queue first: fastest and preserves locality.
  if (kqueue_operation_base* operations = local_state_.pop_cpu_all())
    return reverse_tasks(operations);

  // 2. Shared CPU queue.
  if (kqueue_operation_base* operations = global_state_->pop_cpu_all())
    return reverse_tasks(operations);

  return nullptr;
}
```

Each level takes the whole batch at once (`pop_cpu_all()` + `reverse_tasks()`
to restore FIFO order), and the run loop stops at the first level that yields
a batch. Because a batch is fetched and executed before the loop checks
again, work never piles up on one thread's stack.

`run_cpu_batch()` is the run-loop wrapper: fetch one batch, execute it, return
whether anything ran.

```cpp
bool kqueue_context::run_cpu_batch() noexcept {
  if (kqueue_operation_base* operations = fetch_cpu_task()) {
    execute_tasks(operations);
    return true;
  }
  return false;
}
```

## 4. Sleep/wake transitions

### 4.1 `begin_wait()` — enter the suspend list

When a worker decides to block in the native poller, it links itself into the
suspend list **before** calling the blocking syscall, so a publisher can find
it there.

```cpp
void kqueue_context::begin_wait() noexcept {
  run_state_.waiting.store(true, std::memory_order_release);
  if (global_state_ == nullptr) return;
  {
    std::lock_guard<std::mutex> guard(global_state_->workers.lock);
    kqueue_link_local_state(global_state_->workers, &local_state_);
  }
  global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
}
```

### 4.2 `end_wait()` — leave the suspend list

Waking up is the mirror image; the worker unlinks itself **before** processing
any work.

```cpp
void kqueue_context::end_wait() noexcept {
  if (global_state_ != nullptr) {
    {
      std::lock_guard<std::mutex> guard(global_state_->workers.lock);
      kqueue_unlink_local_state(global_state_->workers, &local_state_);
    }
    global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  }
  run_state_.waiting.store(false, std::memory_order_release);
}
```

### 4.3 Lock ordering

There is a single list lock (the suspend list's), so there is no lock
ordering cycle between workers. The `submit_lock` (held by publishers and
`stop()`) is acquired before the suspend lock in
`wake_one_sleeping_locked()`; no path takes `submit_lock` while holding the
suspend lock, so there is no cycle there either.

## 5. Directed wakeup: `wake_one_sleeping()`

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
  kqueue_worker_state_list& suspend = workers;
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

### 5.1 Timer wakeups: `wake_one_if_all_workers_sleeping()`

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

## 6. Lifecycle: unregister

```cpp
void kqueue_context::unregister_local_state() noexcept {
  if (global_state_ == nullptr) return;
  // Unlink from the suspend list if the worker is still linked there (a
  // run() that exits through the normal wait path already unlinked in
  // end_wait()).
  std::lock_guard<std::mutex> guard(global_state_->workers.lock);
  if (kqueue_local_state_in_list(global_state_->workers, &local_state_)) {
    kqueue_unlink_local_state(global_state_->workers, &local_state_);
  }
}
```

`unregister_local_state()` runs after the run loop exits, **before** the
native context (and its `local_state_`) is destroyed. This ordering is the
foundation of the UAF guarantee: any thread holding the suspend list lock is
either before unregister (node alive) or after it (node already unlinked and
no longer reachable).

## 7. I/O handling

`publish_io()` mirrors `post()`: an operation published from a callback that is
running on this context's run loop goes to the worker's own I/O queue
(`local_state_.push_io()`); anything else goes to the shared I/O queue
(`global_state_->push_io()`) and wakes a worker. `consume_io_tasks()` drains the
local I/O queue first and only then the shared one, taking the whole batch at
once — the same "first non-empty source wins" order `fetch_cpu_task()` uses.
Batch unfairness for the shared queue is accepted: io_uring and kqueue are
batch-capable, and a worker that cannot handle a batch simply leaves it for the
next fetch pass. There is no I/O stealing, on either queue.

Standalone kqueue contexts (no `global_state_`) always take the local path; the
io_uring backend has no standalone mode. When the io_uring submission queue is
full, `consume_io_tasks()` stashes the operations it could not prepare in a
retry slot owned by the run-loop thread (`pending_io_retry_`), so the next
run-loop pass retries them before popping any queue instead of retrying inline
or pushing them back onto the local I/O queue. Note the ordering consequence:
the retry slot takes strict priority over I/O published after the SQ filled —
with the old local-queue parking, operations pushed onto the local queue behind
the parked batch would have been handled first.

The retry-slot contract on submit failure: a failed submit fails only the
operations that took part in it. Operations already prepared into SQEs inherit
the submit's errno through `fail_io_list()`; the unprepared remainder never
reached an SQE, so it must not be handed an errno it did nothing to produce.
The remainder keeps its retry-slot turn whenever the submit error is one the
next pass can plausibly clear — `EINTR`, `EAGAIN`, `EBUSY`, or `ENOMEM` (the
transient submit errors of `io_uring_enter(2)`). Any other submit error fails
the remainder with the same errno instead of re-queueing it: a ring that
rejects every submit would otherwise spin the retry slot forever without ever
reaching the fatal-error routing.

## 8. Why the structure is shaped this way

- **Lock and list head in one struct.** Every traversal is
  `lock(list) { walk list }` — there is no invariant that depends on which
  other list the same node might also be in.
- **One list instead of two.** The run list existed only for work stealing;
  without stealing, a worker's local state is never touched by remote threads
  while it runs, so only sleeping workers need to be reachable. A single
  suspend list halves the lock traffic on every sleep/wake transition.
- **Doubly-linked with a per-list lock.** Unlinking a node is O(1) given the
  `prev` link, which matters for `begin_wait()`/`end_wait()`/unregister on a
  per-publish or per-wake path.
- **Round-robin fairness via cursor.** The suspend list keeps a `cursor` so
  `wake_one_sleeping()` resumes after the last woken worker. Head insertion
  never invalidates it (existing node addresses do not move), and the cursor's
  validation scan handles the only invalidation case — the cursor's node being
  unregistered.
- **Non-atomic local queue.** Because no remote thread ever reads the local
  CPU queue, its head is a plain pointer. Push and pop are single
  read-modify-write of one pointer with no atomics or fences.

## 9. Run-loop integration

The run-loop phase machine uses `run_cpu_batch()` as its single CPU-work
entry:

```
handle_run_ready_tasks:
    collect ready native events (non-blocking)
    run_cpu_batch()                    → local → shared, one batch
    consume_timeout_operations()
    consume_io_tasks()                 → local → shared, one batch
    wait for work (spin, then block)
```

I/O follows the same local-then-shared order as CPU work: `consume_io_tasks()`
pops the worker's own I/O queue first and falls back to the shared I/O queue
only when the local one is empty. A batch the worker cannot prepare (io_uring
SQ full) is stashed in the run-loop retry slot and retried by the next pass
before any queue is popped, rather than retried inline. On stop,
`abort_inflight_io()` drains the retry slot and **both** I/O
queues and completes everything left with `-ECANCELED` /
`complete_submit_stopped()`, so operations still held anywhere — including
those a `finish()` callback published — are delivered instead of leaked.

The blocking wait path is:

```
wait_for_io_work:
    begin_wait()                       → link into suspend list
    recheck events / cpu / timer / io  → end_wait() + rerun if any work
    compute deadline from timer heap
    block in native poller (both wake fds registered)
    end_wait()                         → unlink from suspend list
    recheck cpu / timer / io           → run_ready_tasks
```

`begin_wait()` is called **before** the recheck so a publisher that lands
during the recheck window finds the worker in the suspend list and can wake
it; `end_wait()` unlinks the worker before it processes any work.

### 9.1 Wake-poll re-arm policy and the no-unbounded-block invariant

Both wake polls are armed by a single helper,
`arm_wake_poll(fd, user_data, pending_flag)`, and the armed/not-armed
distinction is explicit. It returns 1 when the poll is armed — either it just
submitted the `IORING_POLL_ADD` SQE (setting `pending_flag`) or `pending_flag`
was already set — 0 when it is **not** armed because the context is stopping,
and a negative errno on submission failure (`-EINVAL` for a closed ring or
channel, `-EAGAIN` when the submission queue has no free slot or the submit
fails). A `0` tells the caller the ring must not be entered unless another wake
source exists. Both polls are armed once in `enter_run()` and re-armed
immediately before the blocking native wait.

There is no inline retry anywhere: `arm_wake_poll()` returns as soon as
submission fails. Every failure in the wait path is classified at the single
policy point in `wait_for_io_work()` (a re-arm failure while collecting an
eventfd CQE is deliberately not terminal there — it is re-handled by that same
point, so transient pressure never half-closes the context):

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
