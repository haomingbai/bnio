# Timer Design

`io_context` timers are divided into two explicit states:

- **active**: a registered timer whose expiry is still in the future. Active
  timers live in the intrusive time heap.
- **inactive**: a registered timer whose expiry has passed. Inactive timers
  live in an intrusive list and complete newly started waits immediately.

This avoids a scan of every registered timer to discover newly submitted
waits.

Implementation types are platform-independent and live in
`include/bnio/detail/posix/io_context/` (shared by the Linux and BSD backends; there
is no platform-specific timer split):

- `timer_types.h` defines `timer_slot`, timer operation types, and
  `timer_state_data`.
- `timer_wait.h` defines the user-facing wait sender and operation.
- `steady_timer.h` owns one `timer_slot`.

## Goals

- Make starting a wait on an active timer O(1) under the timer mutex.
- Make starting a wait on an expired timer enqueue a local-loop completion
  without using the shared CPU task queue.
- Keep each registered timer in exactly one intrusive scheduling container.
- Eliminate timer-registry scans, per-timer mutexes, generation counters, and
  atomic timer-operation queues.
- Keep cancellation, expiry replacement, and timer destruction asynchronous:
  receivers are never called while timer state is locked.
- Let native workers consume timer deadlines passively while deciding how long
  to sleep; no timer owns a native timeout submission.

## Timer Slot Layout

Each `steady_timer` owns one `detail::timer_slot`.

| Field | Role |
|---|---|
| `context` | Owning context while registered; null after unregistration. |
| `expiry` | Absolute deadline selected by `expires_at()` / `expires_after()`. |
| `submitted.head` | First pending active-timer wait. |
| `submitted.size` | Number of queued waits, used by `cancel()` and expiry replacement without traversing the queue under the mutex. |
| `previous`, `child`, `next` | Intrusive heap/list topology links. |
| `active` | `true` only while the slot belongs to the active time heap. |

`timer_state_data::ready` is a separate intrusive list of waits that have
already selected value, canceled, or stopped completion. It is drained only
by a native worker's passive timer check.

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
- the submitted head-linked queue (`head` and `size`);
- the pending timer-completion list consumed by native loop checks.

There is no per-timer mutex, atomic submitted-operation pointer, reusable
driver operation, or native timeout state machine.

`timer_state_data::timeout_fetching` is a small atomic admission gate for
workers that check timers. At most one worker attempts the structural mutex at
a time; other workers skip that loop check instead of producing avoidable
`try_lock()` contention.

`steady_timer` remains externally serialized for concurrent mutations of the
same object, as documented on the public type.

## Waiting

Starting a timer wait has two paths:

```text
lock timer mutex
  timer inactive -> add this operation to the timer-ready list

  timer active   -> insert operation at submitted head
                    keep no additional state and do not wake a worker
unlock timer mutex
```

An active timer is already in the time heap, so its deadline is already known
to the backend. Registering a wait therefore does not need to scan timers,
change heap topology, or enqueue a task.

Wait registration is a head insertion:

```cpp
operation.timer_next_ = timer.submitted.head;
timer.submitted.head = &operation;
++timer.submitted.size;
```

This removes tail maintenance and preserves the timer subsystem's intrusive,
allocation-free registration path. An expired, token-cancelled, or aborted
operation first enters the timer-ready list; a native worker transfers that
list to its own local CPU queue during its next loop check.

## Expiry Replacement and Cancellation

`expires_at()` / `expires_after()` first take the timer mutex and:

1. Remove the slot from its current heap/list container.
2. Store the new expiry.
3. Detach the one submitted head-linked queue.
4. Mark the detached operations canceled (they deliver
   `set_value(operation_canceled)`) and link them into the timer-ready
   list.
5. Insert it into the active heap or inactive list according to the new time.
6. Release the mutex.

The return value is the detached list's stored `size`, so cancellation and
expiry replacement do not walk the task list while holding the mutex.

`cancel()` only detaches and queues the submitted head-linked list. It does not change the
timer's active/inactive state or expiry.

## Expiry Processing

The backend processes due heap roots as follows:

1. Pop the earliest active timer from the pairing heap.
2. Detach its submitted head-linked queue.
3. Mark it inactive and link it into the inactive list.
4. Add the queue to the timer-ready list with value completion.
5. Return that ready list to the native loop, which links it into the current
   worker's local CPU queue.

This state transition is performed even if the queue is empty. Therefore a
subsequent wait on an expired timer observes the inactive state and takes the
direct-post path.

Both native backends call `try_fetch_timeout_operations()` on every loop pass
and again while selecting a blocking timeout. The callback first uses the
atomic admission gate and then takes the timer mutex without blocking. It
returns due/completed waits as a native operation list plus the current heap
deadline. The worker links that list directly into its local CPU queue and
uses the deadline as its passive sleep timeout: `io_uring_wait_cqe_timeout()`
on Linux and the `kevent()` timeout argument on BSD.

## Passive Deadline Integration

No timer completion is injected as a native I/O request. Instead, the backend
follows this sequence:

```text
worker is about to sleep
  -> non-blockingly fetch timer-ready operations and next heap deadline
  -> transfer ready operations to this worker's local CPU queue
  -> sleep until native I/O, explicit wakeup, or the heap deadline
```

When a newly registered/rearmed timer becomes the earliest deadline, or a
mutation adds ready completions, the context wakes one worker only if all
published workers are sleeping. Starting a wait on an already active timer
only links the operation; it does not need a wakeup. Removing an earliest
timer does not require a wakeup: the old passive timeout may fire once and the
next loop check will select the new deadline.

## Destruction and Debug Builds

Unregistration removes a timer from its heap/list and places only actual
pending waits on the timer-ready list with canceled completion
(unregistration is a timer-object abort, so those waits deliver
`set_value(operation_canceled)`). It does not
submit a native timer operation merely to recompute a deadline, because
passive deadline selection happens naturally on the next worker loop check.

When `io_context::stop()` is called, `begin_stop()` first calls
`abort_pending_timer_waits()` — **before** taking `submit_lock` and
publishing `life_state = 1`.  The abort iterates every active and
inactive timer slot, detaches each slot's pending submitted waits, marks
them `timer_completion_kind::canceled`, and enqueues them into
`timers_.ready`.  Only then is the stopping state published, so every
native worker that later observes `life_state != 0` and enters its
`finish()` path sees a fully populated `timers_.ready` and drains those
canceled completions via `consume_timeout_operations()` during its Phase 1
drain loop.  Every receiver waiting on a timer receives
`set_value(operation_canceled)` — a context-stop abort is not token
cancellation.

This ordering — abort before the stopping-state publication — is the
happens-before guarantee: the release store of `life_state` on the stop
thread is sequenced after `abort_pending_timer_waits()` in program order,
and any worker's acquire load of `life_state` that observes the non-zero
value synchronizes-with that store.  Workers that are actively spinning
(not sleeping in a syscall) are therefore guaranteed to see the aborted
operations on `timers_.ready` before they enter their final drain,
closing the window where a worker could observe the stopping state,
drain the still-empty `timers_.ready`, and exit before the abort staged
the operations — which would permanently strand them with no worker left
to drain.

## Completion Kinds and Channels

Timer completions carry one of three kinds, and
`timer_wait_operation::execute()` maps each kind to exactly one receiver
call (`include/bnio/detail/posix/io_context/timer_wait.h:40`):

| Kind | Staged by | Delivered as |
|---|---|---|
| `stopped` | `start()` token pre-check: the receiver's stop token is already cancelled (`include/bnio/detail/posix/io_context/timer_wait.h:24`) | `set_stopped()` |
| `canceled` | `cancel()`, expiry replacement, destruction unregistration, and the context-stop abort `abort_pending_timer_waits()` | `set_value(operation_canceled)` |
| `value` | expiry processing | `set_value({})` |

`set_stopped` is therefore reserved for observed token cancellation; every
timer-object abort and every context-stop abort delivers
`value(operation_canceled)`, matching the unified stop contract documented
in [`lifecycle.md`](lifecycle.md).

Timer aborts do not re-arbitrate at `execute()` time — unlike I/O
operations, whose `execute()` arbitrates between the token and a
context-stop abort. The kind is fixed when the completion is staged: under
the timer mutex for object-API cancellation, and on the stopping thread for
`abort_pending_timer_waits()`, which runs before `life_state` is
published. No staging site can race a later channel decision, so the
delivery channel is decided exactly once, at the observation point where
the abort happened.

## Invariants

- Every registered timer belongs to exactly one of the active heap or inactive
  list.
- `active == true` means the slot is reachable from `timer_state_data::heap`.
- `active == false` and `context != nullptr` means the slot is reachable from
  `timer_state_data::inactive`.
- At most one submitted head-linked queue exists per timer.
- Queue `head` and `size` are either both empty/zero or describe the same
  intrusive list.
- The timer mutex protects every queue and topology mutation, including the
  timer-ready list.
- Timer completion is transferred into a local run-loop queue only after the
  timer mutex is released; receiver code is never invoked by heap/list
  maintenance.
- The active-wait path never scans all registered timers.
