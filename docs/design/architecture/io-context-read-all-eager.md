# io_context read-all and eager-optional design

> Status: finalized. This is the design basis for the three-commit refactor
> (commit A/B/C).

## 1. Terminology: probe semantics vs initiation semantics

- **Probe semantics (`start_io`)** — the eager immediate-completion entry point.
  Before registering with kqueue/io_uring it performs one non-blocking syscall
  (recv/send/accept/connect/pread/pwrite) and also carries out the required
  setup (fcntl O_NONBLOCK, fstat to classify regular files). A result other
  than -EAGAIN completes immediately.
- **Initiation semantics (`perform_io`)** — the readiness-driven entry point.
  Triggered by a kqueue kevent / io_uring CQE. It assumes the prerequisites are
  already satisfied and performs the I/O syscall directly. If it returns
  -EAGAIN, the kqueue path re-arms and keeps waiting.

The core difference is whether one attempt is made before the kernel readiness
registration, and who is responsible for the setup.

## 2. Current state (BSD / Linux)

| Path | eager mechanism | accept/connect eager? |
|---|---|---|
| io_context high-level BSD (`detail/bsd/io_context_native_io/common.h` `native_io_operation::start`) | calls `request_.start_io()` unconditionally | yes (`::connect()` is issued only from `start_io`) |
| kqueue low-level (`kqueue_ready_io_operation`) | gated by the `has_start_io` concept | yes |
| io_context high-level Linux (`detail/linux/io_context_native_io/common.h`) | gated by the `has_immediate_io` concept over `try_immediate()` | no (accept/connect have no `try_immediate`; they go straight into io_uring) |
| io_uring low-level | none | no |

**Key finding (verified by a macOS kqueue probe)**: for a regular file,
`EVFILT_READ` registration succeeds but **never fires** (the probe observed
n=0 within 100ms); `EVFILT_WRITE` fires immediately. Therefore a non-eager
regular-file read **cannot** rely on kqueue readiness waiting; it must go
straight through `perform_io()` to completion.

## 3. Commit A: async_read moves to read-all / repeat_until

- `read_all.h` already exists but was never included. It reuses the
  `write_all.h` templates; only the State classes differ.
- **Zero-byte termination split**: for write-all, `bytes == 0` is a
  broken_pipe error (the write peer closed). For read-all, `bytes == 0` is
  EOF and terminates **normally** with `ec={}` and `bytes = transferred`
  (possibly 0). This is expressed by a `static constexpr bool
  zero_byte_is_error` on each State so that only one template family exists.
- `io_context::async_read` (socket + descriptor) and the `basic_scheduler`
  variants move to `read_all_sender`; `async_read_some` stays single-shot.
- EOF semantics: a `recv`/`pread` returning 0 terminates the loop with
  `ec={}`; a first-step error propagates its `ec` and terminates; a
  zero-length buffer completes synchronously with `ec={}, 0`.
- Call sites that relied on single-read `async_read` (tests/examples/
  benchmarks/docs) move to `async_read_some`.

## 4. Commit B: eager becomes optional (compile time)

**Mechanism**: `EnableImmediate` (default `true`) is threaded as a template
parameter through:

1. The high-level `native_io_operation`/`native_io_sender` (one pair for BSD,
   one for Linux).
2. The `io_context::async_*` and `basic_scheduler::async_*` member functions
   (`template <bool EnableImmediate = true>`), so tests can explicitly select
   the non-eager mode with `scheduler.async_read_some<false>(...)`.
3. The `write_all.h`/`read_all.h` State classes become
   `template <bool EnableImmediate>` templates so every inner step of a
   composite honors the switch.

**Non-eager semantics (BSD)**: `start()` skips `start_io()` and instead calls
`request_.perform_io()` once; on -EAGAIN it publishes to kqueue and waits, and
the kevent fires `perform_io()` again; otherwise it completes immediately.
This means:
- regular-file I/O completes directly (no dependence on the EVFILT_READ that
  never fires);
- a socket's first syscall returns EAGAIN and then correctly uses the kqueue
  readiness path;
- connect's first call issues `::connect()`, ruling out a "false success".

**BSD request rework**:

- `kqueue_accept_request`: `start_io()` drops the `set_nonblocking` fallback
  on the listener (the acceptor constructor already guarantees O_NONBLOCK;
  no leeway); flag validation moves into `perform_io()`; the accepted fd is
  still set O_NONBLOCK.
- `kqueue_connect_request`: reworked into a **reentrant two-phase state
  machine**. `perform_io()`:
  - not yet initiated: set O_NONBLOCK + `::connect()`; 0/EISCONN → success;
    EINPROGRESS/EALREADY → mark initiated and return -EAGAIN (re-arm and
    wait for EVFILT_WRITE); any other errno → error.
  - initiated: `getsockopt(SO_ERROR)`; still EINPROGRESS → -EAGAIN to keep
    waiting; 0/-errno → complete.
  `start_io()` (eager) goes through the same state machine.
- `kqueue_file_read_request`/`write_request`: the fstat moves from
  `start_io()` into `perform_io()` as a one-time lazy resolution, making
  `perform_io()` self-sufficient in non-eager mode.

**Linux**: accept/connect never had eager; `EnableImmediate=false` affects
socket read/write/file (skips `try_immediate` and goes straight to io_uring).
The kqueue low-level `kqueue_ready_io_operation` does not get the switch (the
io_context-layer tests do not depend on it), but the request rework is
behavior-preserving for the low level.

## 5. Commit C: compile time → run time

- `io_context_options` gains an immutable bool `enable_immediate_io = true`
  (embedded in the options, not atomic).
- `io_context` stores it at construction and exposes a read-only accessor
  `enable_immediate_io()`.
- The high-level `native_io_operation::start()` reads the value at run time
  to decide eager vs non-eager; the hot path is a single bool read with zero
  overhead.
- The `EnableImmediate` template parameters are removed from the public API;
  the State classes revert to plain classes (inner steps are driven by the
  run-time switch).
- Commit B's tests move from the `<false>` template syntax to the
  `io_context_options` option.

## 6. Key decisions

- Non-eager first probes with `perform_io()` and waits on EAGAIN rather than
  unconditionally publishing: because the regular-file EVFILT_READ never
  fires on macOS, and for sockets the behavior is equivalent and correct.
- connect keeps the `set_nonblocking` fallback (unlike accept): in non-eager
  mode `perform_io()` still ensures O_NONBLOCK before issuing `::connect()`,
  so the worker never blocks.
- Commit messages use the `refactor:` prefix; the three commits are
  independently revertable.

## 7. Platform verification matrix

- Commit A: full macOS ctest.
- Commits B/C: full macOS ctest + full devcloud (Linux/io_uring) ctest.
- devcloud flow: `tar` (excluding build/.git) → `scp` → `cmake -B build
  -DBNIO_BEXEC_PROVIDER=SOURCE -DBNIO_BEXEC_SOURCE_DIR=~/my_projects/bexec` →
  build → `ctest --output-on-failure` (the devcloud bexec checkout matches
  local commit 52ad5f4).
