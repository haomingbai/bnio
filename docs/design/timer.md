# Timer Design

`io_context` timers are divided into two explicit states:

- **active**: a registered timer whose expiry is still in the future. Active
  timers live in the intrusive time heap.
- **inactive**: a registered timer whose expiry has passed. Inactive timers
  live in an intrusive list and complete newly started waits immediately.

This avoids the previous hot path in which a timer driver had to scan every
registered timer to find newly submitted waits.

Implementation types live in the matching
`include/bnio/{linux,bsd}/detail/` directory:

- `io_context_timer_types.h` defines `timer_slot`, timer operation types, and
  `timer_state_data`.
- `io_context_native_io/timer_wait.h` defines the user-facing wait sender and
  operation.
- `steady_timer.h` owns one `timer_slot`.

## Goals

- Make starting a wait on an active timer O(1) under the timer mutex.
- Make starting a wait on an expired timer a direct native-context post.
- Keep each registered timer in exactly one intrusive scheduling container.
- Eliminate timer-registry scans, per-timer mutexes, generation counters, and
  atomic timer-operation queues.
- Keep cancellation, expiry replacement, and timer destruction asynchronous:
  receivers are never called while timer state is locked.
- Reuse the existing driver and native timeout operations.

## Timer Slot Layout

Each `steady_timer` owns one `detail::timer_slot`.

| Field | Role |
|---|---|
| `context` | Owning context while registered; null after unregistration. |
| `expiry` | Absolute deadline selected by `expires_at()` / `expires_after()`. |
| `submitted.head` | First pending active-timer wait. |
| `submitted.tail` | Last pending active-timer wait, enabling O(1) FIFO append. |
| `submitted.size` | Number of queued waits, used by `cancel()` and expiry replacement without traversing the queue under the mutex. |
| `previous`, `child`, `next` | Intrusive heap/list topology links. |
| `active` | `true` only while the slot belongs to the active time heap. |

The topology metadata is three pointers and one Boolean:

```text
active timer:
  previous = parent for the first child, or previous sibling otherwise
  child    = first child
  next     = next sibling

inactive timer:
  previous = previous inactive timer
  child    = null
  next     = next inactive timer
```

The active form is an intrusive pairing min-heap. Its `previous` link lets an
arbitrary slot be cut from its parent/sibling chain in O(1) link updates. The
same storage becomes a doubly linked inactive list after expiry, so no
allocation or separate registry is needed.

## Active and Inactive State

A registered slot has one of these states:

| State | `context` | `active` | Container |
|---|---:|---:|---|
| Active | non-null | `true` | pairing min-heap |
| Inactive | non-null | `false` | inactive list |
| Unregistered | null | `false` | none |

When a timer is registered or its expiry is changed:

```text
expiry > clock::now()  -> active: insert into time heap
expiry <= clock::now() -> inactive: insert into inactive list
```

Changing an inactive timer to a future expiry first removes it from the
inactive list and then inserts it into the heap. Changing an active timer
removes its exact heap node before re-inserting it at the new deadline.

## Synchronization

The timer subsystem has one structural mutex:

```text
io_context::timers_.mutex
```

It protects:

- active-heap and inactive-list topology;
- `timer_slot::context`, `expiry`, and `active`;
- the complete submitted FIFO queue (`head`, `tail`, and `size`);
- reusable native timeout state.

There is no per-timer mutex and no atomic submitted-operation pointer. The
only timer-related atomic is `timer_state_data::driver`, which prevents
duplicate posts of the reusable driver operation.

`steady_timer` remains externally serialized for concurrent mutations of the
same object, as documented on the public type.

## Waiting

Starting a timer wait has two paths:

```text
lock timer mutex
  timer inactive -> unlock
                    post this operation with value completion

  timer active   -> append operation to submitted FIFO
                    keep no additional state and do not post the driver
unlock timer mutex
```

An active timer is already in the time heap, so its deadline is already known
to the backend. Appending a wait therefore does not need to scan timers,
change heap topology, or enqueue a driver task.

The queue is FIFO:

```cpp
operation.timer_next_ = nullptr;
if (timer.submitted.tail != nullptr) {
  timer.submitted.tail->timer_next_ = &operation;
} else {
  timer.submitted.head = &operation;
}
timer.submitted.tail = &operation;
++timer.submitted.size;
```

At expiry, the driver can detach this entire queue and concatenate it with the
current completion batch in O(1), using the stored tails. Receiver execution
is performed only after releasing the mutex.

## Expiry Replacement and Cancellation

`expires_at()` / `expires_after()` first take the timer mutex and:

1. Remove the slot from its current heap/list container.
2. Detach the one submitted FIFO queue.
3. Store the new expiry.
4. Insert it into the active heap or inactive list according to the new time.
5. Release the mutex.
6. Post every detached operation with `set_stopped()`.

The return value is the detached queue's stored `size`, so cancellation and
expiry replacement do not walk the task list while holding the mutex.

`cancel()` only detaches and posts the submitted FIFO. It does not change the
timer's active/inactive state or expiry.

## Expiry Processing

The backend processes due heap roots as follows:

1. Pop the earliest active timer from the pairing heap.
2. Detach its submitted FIFO queue.
3. Mark it inactive and link it into the inactive list.
4. Add the queue to a local completion batch.
5. After releasing the timer mutex, post that batch with value completion.

This state transition is performed even if the queue is empty. Therefore a
subsequent wait on an expired timer observes the inactive state and takes the
direct-post path.

On Linux, `timer_driver_operation_` performs this work after an io_uring
timeout completion and then arms or updates the next native timeout. On BSD,
`try_fetch_timeout_operations()` performs the same transition before the
kqueue worker sleeps.

## Driver and Native Timeout State

The reusable driver remains protected by this atomic state machine:

```text
idle -> posted -> idle
```

Only expiry changes, timer registration of a future deadline, and native
timeout completion need to post the driver. Starting a wait on an already
active timer does not.

Linux retains the existing single-timeout state machine:

```text
idle -> armed -> updating -> armed
                  \-> update_pending -> idle
```

The heap root supplies the next deadline. A stale native timeout after timer
removal is harmless: it causes at most one extra driver pass and never
completes a user wait incorrectly.

## Destruction and Debug Builds

Unregistration removes a timer from its heap/list and posts only actual
pending waits with stopped completion. It deliberately does not post a timer
driver merely to recompute a deadline. This is important after `run()` has
returned: the native context may already be `finished`, and posting a
no-op driver would violate its Debug `assert_running()` precondition.

Once `io_context::stop()` begins closing the context, timer driver and
completion posts are suppressed as well. There is no runnable context left to
consume them, and suppressing them prevents a shutdown/destruction path from
posting into a finished native context.

## Invariants

- Every registered timer belongs to exactly one of the active heap or inactive
  list.
- `active == true` means the slot is reachable from `timer_state_data::heap`.
- `active == false` and `context != nullptr` means the slot is reachable from
  `timer_state_data::inactive`.
- At most one submitted FIFO queue exists per timer.
- Queue `head`, `tail`, and `size` are either all empty/zero or all describe
  the same intrusive list.
- The timer mutex protects every queue and topology mutation.
- Timer completion is always posted after unlocking; receiver code is never
  invoked by heap/list maintenance.
- The active-wait path never scans all registered timers.
