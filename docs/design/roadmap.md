# Roadmap

This document describes the next phase of `bnio` development. It replaces the
former `kqueue-roadmap.md`, which tracked the BSD port — that port is now
complete and shipped.

The next phase has one committed direction and one open branch:

1. **POSIX `io_context` consolidation** — the committed next step. Merge the two
   structurally-mirrored native backends into one POSIX-shared implementation.
2. **Memory-customization strategy** — an open branch with two mutually
   exclusive options: templatize the library with allocator support, or adopt
   PMR. A decision is deferred; this document presents both so the trade-offs
   can be compared.

## Current state (ground truth)

The high-level runtime is already platform-neutral. `io_context`,
`basic_scheduler`, `schedule_sender`, the intrusive timer heap, the shared
MPSC CPU/I/O task queues, the wake channel, and `steady_timer` are written once
under `detail/io_context/` and shared by both backends
(`include/bnio/detail/io_context/class.h:56-586`,
`src/io_context*.cpp`). The platform boundary is confined to three places:

| # | Boundary | Location |
|---|----------|----------|
| 1 | Macro-dispatched type aliases | `detail/io_context/native_context.h:22-40` selects `io_uring_context` or `kqueue_context`, plus the `native_operation_base` / `native_io_operation_base` / `native_task_queue_state` aliases. |
| 2 | Backend request factories | `detail/{linux,bsd}/io_context_native_io/` — sender/factory headers. BSD has `common.h` + `factories.h`; Linux adds `file.h`, `poll.h`, `socket.h`. |
| 3 | Two mirrored run-loops + task state | `async_io/bsd/kqueue_context_base/` and `async_io/linux/io_uring_context_base/` — `*_context`, `*_task_queue_state`, `*_operation_base` are field-for-field identical but independently maintained. |

The key enabler for consolidation: **the kqueue backend already emulates the
io_uring proactor model.** In `dispatch_event_result`
(`src/async_io/bsd/kqueue_context_events.cpp:111-118`), a readiness event drives
a nonblocking `perform_io()`; on `-EAGAIN`/`-EWOULDBLOCK` the operation is
re-armed via `try_rearm_operation()` and stays inflight. io_uring skips that
step only because the kernel has already performed the I/O
(`src/async_io/linux/io_uring_context_cqe.cpp:106-121`). Both backends expose
the same `owns_io_step()` / `perform_io()` / `prepare()` contract to the shared
factory layer, so a shared proactor facade does not require rewriting any I/O
path.

This is also why the consolidation is scoped to POSIX: kqueue can stand in for
io_uring because both translate to the same readiness → nonblocking-retry shape.
A proactor built on overlapped I/O with kernel-side completion (e.g. IOCP) does
not map onto that shape — its completions carry performed I/O rather than
readiness, and its submission model is per-operation rather than
readiness-registered. Such a backend cannot share this proactor facade and would
need its own implementation. The POSIX layer is therefore deliberately not
claimed to be a universal abstraction; it is a POSIX-shared one.

---

## 1. POSIX `io_context` consolidation

### Goal

Collapse boundary #3 (and most of #1) into a single POSIX-shared
implementation. The public `io_context`, schedulers, timers, queues, and wake
mechanism stay where they are; the two mirrored native context classes merge
into one `posix_context` that embeds either `base::kqueue` or `base::ring`.

### Why now

`kqueue_task_queue_state` and `io_uring_task_queue_state` are field-for-field
identical (`cpu_head`, `io_head`, `awake_workers`, `running_workers`, `closing`,
`timeout_heap`, `try_fetch_timeout_operations`, `wake_channel_`); only the
element type name differs. The `*_operation_base` classes are equally isomorphic
(`next`, `result`, `flags`, virtual `execute()`). The run-loop phase machines
(`run()`, `handle_run_ready_tasks()`, `handle_wait_for_work()`,
`spin_for_work()`, `wait_for_io_work()`, `should_finish()`, `finish()`) are
line-for-line corresponding between
`src/async_io/bsd/kqueue_context_run_loop.cpp` and
`src/async_io/linux/io_uring_context_run_loop.cpp`. Maintaining two copies of
the same structure is the largest source of drift risk in the codebase.

### Scope

- **Merge task-queue and operation bases.** Unify `kqueue_task_queue_state` and
  `io_uring_task_queue_state` into one `posix_task_queue_state`; unify
  `*_operation_base` and `*_io_operation_base`. The kqueue backend's
  `registrations[]` / wait-queue node stays as a backend-attached sub-object
  rather than a reason to keep the base class split.
- **Template the run-loop phase machine.** Lift `run()` and the phase helpers
  into a shared facade/template. The only pieces that remain platform-specialized
  are:
  - event collection — `kevent()` on BSD vs. `io_uring_enter()` / `wait_cqe` on
    Linux;
  - the blocking wait — the `kevent()` timeout argument vs.
    `io_uring_wait_cqe_timeout()`;
  - registration primitives — `EV_ADD` / `EV_DELETE` vs. SQE submission.
- **Single `posix_context`.** One class embeds `base::kqueue` (BSD) or
  `base::ring` (Linux) behind the same proactor contract. `native_context.h`
  then aliases `native_context = posix_context` regardless of platform, removing
  boundary #1's per-backend context type.
- **Merge the request factories.** `detail/{linux,bsd}/io_context_native_io/`
  already expose matching `native_io_operation` / `native_poll_operation` /
  `resolve_operation` signatures. Parameterize one shared factory by the
  proactor `prepare()` / `perform_io()` contract instead of maintaining two
  trees. The BSD regular-file blocking-at-start fallback
  (`start()` performs positioned `pread()`/`pwrite()`, then posts the
  completion) is preserved as a backend behavior, not a separate factory type.

### Non-goals

- Do not change the public `io_context`, scheduler, timer, TCP, UDP, DNS, or TLS
  APIs.
- Do not introduce a new abstraction layer above the POSIX backends. The result
  is one shared implementation, not a plugin interface.
- Do not pursue non-POSIX backends in this phase. As noted above, an overlapped
  I/O completion model cannot share this facade and is out of scope here.

### Acceptance

- `detail/io_context/native_context.h` no longer defines separate
  `io_uring_context` / `kqueue_context` aliases for the context type itself —
  both platforms use `posix_context`.
- The run-loop phase machine exists in one place; BSD- and Linux-specific code
  is limited to event collection, the blocking wait, and registration
  primitives.
- All existing Linux and BSD integration tests pass without backend-specific
  test changes.

---

## 2. Memory-customization strategy (open branch)

The runtime already honors a hard constraint: **when no allocator is provided,
hot paths do not allocate.** Every runtime queue — MPSC CPU queue, MPSC I/O
queue, inflight doubly-linked list, kqueue wait-queues, timer pairing-heap,
timer ready/inactive lists, per-slot submitted queue — is an intrusive
structure embedded inside operation or timer objects, with push/pop implemented
as inline pointer swaps or CAS. There are no `std::vector` / `std::deque` /
`std::map` / `std::unordered_map` / `std::function` on any run-loop path. The
sole runtime allocation is a single setup-time
`new (std::nothrow) bnio::base::event[]` in `kqueue_context::queue_init`
(`src/async_io/bsd/kqueue_context.cpp:45-46`); the io_uring side allocates CQEs
through the kernel-mmap'd ring with no userspace heap allocation.

Given that, the question is how to let callers customize the few allocations
that do exist (and any future ones). There are two mutually exclusive options.
**A decision has not been made;** this section exists to compare them.

### Option A — Templatize with allocator support

Add a template `Allocator` parameter to the runtime types and route the few
allocations through `std::allocator_traits<Allocator>`.

What it touches:

- The `Allocator` parameter propagates through `io_context`, the native context,
  `*_task_queue_state`, `*_operation_base`, `steady_timer`, `timer_state_data`,
  and every `*_operation<Request,Receiver>` / `*_sender` class — roughly 30+
  types — plus the `native_context.h` alias layer.
- The only allocation to actually reroute is `kqueue_context::event_buffer_`.
  io_uring needs no hot-path change (kernel-mmap'd ring).

Trade-offs:

- **Pro:** explicit, compile-time, zero-overhead allocation control; composes
  with custom allocators and existing STL allocator-aware patterns.
- **Con:** invasive — a template parameter spreads through the entire operation
  hierarchy even though the hot paths allocate nothing. Binary size and compile
  time grow; the public API gains a parameter most users will leave at default.
  The benefit is small relative to the surface area touched, because the
  no-allocator constraint already makes the hot paths allocation-free.

### Option B — Adopt PMR

Store a `std::pmr::memory_resource*` on `io_context` and propagate it to the
native context via `set_global_state()` or a new `*_context_options` field.

What it touches:

- `io_context` stores the `memory_resource*`; `kqueue_context_options` /
  `io_uring_context_options` gain a field.
- `event_buffer_` becomes a `std::pmr::vector<bnio::base::event>` or a
  `monotonic_buffer_resource`-backed allocation.
- Operation types stay non-templated (PMR is type-erased); the intrusive
  linked-list machinery is unchanged.
- io_uring needs no hot-path change.

Trade-offs:

- **Pro:** localized — touches `event_buffer_`, the context-options structs, and
  the `io_context` constructor. The operation hierarchy and the public API are
  untouched. Matches the "type-erased resource, no template explosion" shape
  that fits a runtime with almost no allocations.
- **Con:** PMR is dynamic (runtime dispatch through `memory_resource*`); it does
  not give compile-time allocator selection. If future hot paths ever need
  per-operation allocation, PMR's type erasure means the allocator does not
  appear in the operation type, which can be a benefit or a loss of static
  control depending on the use case.

### Decision criteria (open)

- If the expected future allocation surface stays tiny and centralized in the
  context, Option B (PMR) reaches the goal with far less churn.
- If there is a need for compile-time, per-type allocator customization
  (including on operation types), Option A is the only one that provides it —
  but at the cost of templating the whole hierarchy.
- Either option preserves the existing no-allocator, allocation-free hot-path
  guarantee; the choice is about how the exceptional allocations are
  parameterized, not whether hot paths allocate.

This branch is intentionally left unresolved. The POSIX consolidation (§1) is
the prerequisite: it removes the duplicated context/task-queue types, so
whichever memory strategy is chosen afterwards is applied once rather than
twice.
