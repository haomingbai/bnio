# Worker Scheduling: Local-State Registry, Stealing, and Directed Wakeup

This document describes how `bnio` schedules CPU work across the threads that
call `io_context::run()`. It covers the two worker-state lists (run/suspend),
the unified `fetch_cpu_task()` path, work stealing, and the directed (single
worker) wakeup that replaces the broadcast wake channel for normal operation.

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
  std::atomic<kqueue_operation_base*> cpu_head;  // MPSC local CPU queue
  kqueue_local_task_queue_state* prev;            // doubly-linked list links
  kqueue_local_task_queue_state* next;
  bnio::base::wake_channel wake_channel_;         // per-worker directed wake
};
```

The local CPU queue is a lock-free MPSC LIFO (CAS push, `exchange` pop). It is
owned by the worker for pushes and pops, and by a remote stealer for
`pop_cpu_all()` while it holds the list lock.

### 2.2 `worker_state_list` — one list + its lock

```cpp
struct kqueue_worker_state_list {
  std::mutex lock;                              // guards the list + node lifetime
  kqueue_local_task_queue_state* head;
  kqueue_local_task_queue_state* cursor;        // round-robin cursor
};
```

The lock and the list head live in the same struct so that every operation on
the list takes exactly the lock that owns it. There is no way to touch a list
node without holding its lock — that is what makes node lifetime safe.

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
  std::atomic<std::size_t> awake_workers;
  kqueue_worker_state_registry workers;              // run + suspend lists
  std::atomic<int> life_state;                       // 0 = running, 1 = stopping
  bnio::base::wake_channel wake_channel_;            // shared broadcast (stop)
  std::mutex submit_lock;                            // publish/stop serialization
};
```

## 3. The fetch path: `fetch_cpu_task()`

Every worker obtains CPU work through one entry point, tried in order:

```
local queue  →  shared CPU queue  →  steal from another worker
```

```cpp
kqueue_operation_base* kqueue_context::fetch_cpu_task() noexcept {
  // 1. Worker-local queue first: fastest and preserves locality.
  if (kqueue_operation_base* operations = local_state_.pop_cpu_all())
    return reverse_tasks(operations);

  // 2. Shared CPU queue.
  if (kqueue_operation_base* operations = global_state_->pop_cpu_all())
    return reverse_tasks(operations);

  // 3. Steal from another worker's local queue.
  return steal_cpu_tasks();
}
```

Each level takes the whole batch at once (`pop_cpu_all()` + `reverse_tasks()`
to restore FIFO order), and the run loop stops at the first level that yields
a batch. Because a batch is fetched and executed before the loop checks
again, work never piles up on one thread's stack; a worker that is truly
idle (both queues empty) is the one that reaches the steal stage.

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

## 4. Work stealing

`steal_cpu_tasks()` runs only when local and shared queues are empty, so the
stealing worker is by definition relatively idle.

```cpp
kqueue_operation_base* kqueue_context::steal_cpu_tasks() noexcept {
  kqueue_worker_state_list& run = global_state_->workers.run;
  std::lock_guard<std::mutex> guard(run.lock);

  // Validate the saved cursor is still in the list, else restart from head.
  kqueue_local_task_queue_state* start = run.head;
  if (steal_cursor_ != nullptr) {
    kqueue_local_task_queue_state* scan = start;
    while (scan != nullptr && scan != steal_cursor_) scan = scan->next;
    if (scan != nullptr) start = steal_cursor_;
    else steal_cursor_ = nullptr;
  }

  // Steal the first non-empty victim's whole local queue; stop immediately.
  kqueue_local_task_queue_state* node = start;
  while (node != nullptr) {
    if (node != &local_state_) {
      if (kqueue_operation_base* operations = node->pop_cpu_all()) {
        steal_cursor_ = node->next;   // next round resumes after this victim
        return reverse_tasks(operations);
      }
    }
    node = node->next;
  }

  steal_cursor_ = nullptr;
  return nullptr;
}
```

Key points:

- **Only the run list is scanned.** A sleeping worker has no local CPU work
  worth taking, and is guarded by the suspend list's lock instead. This keeps
  the two lists' locks independent: stealing never takes the suspend lock.
- **UAF protection.** The victim unregisters its `local_state_` under the run
  list lock before its context is destroyed. A stealer holding the same lock
  therefore never dereferences a freed node.
- **Fairness via a per-worker cursor.** `steal_cursor_` is a private member of
  each `kqueue_context`. It records where the last successful steal left off
  so the next round begins after that victim instead of always at the head.
  Head insertion never moves an existing node, so a valid cursor stays valid
  without re-traversal. If the cursor's node was unregistered, the validation
  scan restarts from the head.
- **Bulk steal, stop at first hit.** The whole victim queue is taken in one
  `pop_cpu_all()`, and traversal stops as soon as one victim yields a batch.
  This matches the "batch, not single-task" policy used by `fetch_cpu_task`.

## 5. Sleep/wake transitions

### 5.1 `begin_wait()` — enter the suspend list

When a worker decides to block in the native poller, it moves itself from the
run list to the suspend list **before** calling the blocking syscall, so a
publisher can find it there.

```cpp
void kqueue_context::begin_wait() noexcept {
  waiting_.store(true, std::memory_order_release);
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
  waiting_.store(false, std::memory_order_release);
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

The shared `wake_channel_` is now reserved for broadcast scenarios — `stop()`
must wake every worker. For normal publication, one sleeping worker is enough.
Each worker therefore listens to **two** wake sources:

1. its **per-worker** `wake_channel_` (directed wake);
2. the **shared** `wake_channel_` (broadcast wake, e.g. stop).

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
  if (!global_state_.wake_one_sleeping()) {
    wake_locked();   // nobody suspended → broadcast
  }
}
```

The directed write is safe against the worker's destruction because the
worker unregisters under the suspend list lock before its context is
destroyed: `wake_one_sleeping()` holds that lock for the whole write, so it
can never write a channel that is about to be closed.

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

Standalone native contexts (no `global_state_`) keep a private `local_io_head_`
as a fallback drained by `consume_io_tasks()`; this path is not used by
`io_context`.

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
- **Round-robin cursors.** Both lists keep a `cursor`; stealing and waking
  resume after the last visited node, spreading load evenly across workers.
  Head insertion never invalidates a cursor (existing node addresses do not
  move), and the validation scan handles the only invalidation case — the
  cursor's node being unregistered.

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
