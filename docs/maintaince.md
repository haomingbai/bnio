# Maintenance Notes

## Naming

- Public types, methods, and free functions use `snake_case`.
- Class member variables use Google Style trailing underscores, such as `ring_`
  and `sqe_`.
- Wrapper method names should align with the `liburing` C API and usually drop
  the `io_uring_` prefix, such as `io_uring_submit` to `ring::submit()`.

## liburing Baseline

- The project depends on `liburing >= 2.1` with compatibility workarounds for
  older headers where feasible.
- Avoid adding wrappers for newer or niche helpers unless the version baseline is
  intentionally raised.

## Base Layer Rules

- `bnio::base` is a thin C API to C++ object mapping.
- On Linux, public wrapper headers declare API; method implementations live under
  `src/base/linux/` and are compiled into `bnio`.
- On BSD, wrapper headers live in `include/bnio/base/bsd/` and implementations
  in `src/base/bsd/`.
- Keep system call return semantics: successful values are non-negative and
  failures are negative `errno` values.
- **I1 — no stranded operations.** Every operation handed to a context
  (`post()` / `publish_io()`) eventually reaches a terminal receiver call
  (`set_value` with an error, or `set_stopped`), regardless of ring/wake-channel
  errors or which thread shuts down. Fatal run-loop errors route through the
  finish drain (drain → abort → deliver), and `queue_exit()` delivers aborted
  completions instead of discarding them.
- **I3 — no unbounded block without a wake source.** The run loop never enters
  an unbounded blocking `io_uring_enter` unless an eventfd poll is armed, a
  bounded timeout bounds the wait, or inflight kernel operations will wake it
  under graceful-stop semantics. Wake-poll re-arms classify `-EAGAIN` as
  transient (retry, never block) and any other failure as fatal (finish drain).
- Base-layer zero-logic transparency audits continue to pass: `bnio::base`
  remains a pure liburing/kqueue wrapper with no scheduling or error policy —
  the run-loop guarantees above are implemented entirely in the async_io layer.
- Do not throw exceptions from base wrapper calls.
- Do not add executors, coroutines, schedulers, or higher-level async models to
  the base layer.
- The base layer does not own file descriptor, buffer, address, path, or message
  lifetimes unless a type explicitly documents ownership.

## Adding Wrappers

- Prefer direct wrappers around stable system call APIs.
- Keep ownership explicit: `ring`, `probe`, and `kqueue` own resources, while
  SQE, CQE, and `event` wrappers are non-owning views.
- Match existing method shape before introducing a new abstraction.
- On Linux, add declarations to `include/bnio/base/linux/` and definitions to
  `src/base/linux/`.
- On BSD, add declarations to `include/bnio/base/bsd/` and definitions to
  `src/base/bsd/`.
- Keep `bnio` buildable as both a static and shared library with
  `BUILD_SHARED_LIBS`.

## Doxygen

- Every public method in a wrapper header should have a Doxygen comment.
- Each wrapper method should include an `@see` reference to the corresponding C
  API or C structure.
- Run `scripts/check-doc.sh` before submitting documentation changes.

## Tests

- Write runtime tests as focused GoogleTest `TEST` cases and register them with
  the shared `bnio_add_gtest` CMake helper.
- Use `GTEST_SKIP()` when a supported backend is unavailable on the host; do
  not silently return from a runtime test.
- Exercise production defaults in behavioral regression tests. Do not clear
  setup flags or weaken concurrency merely to make a test pass; a test that
  intentionally targets a non-default mode must name and assert that mode.
- Keep base-layer Linux tests under `tests/base/linux/`.
- Keep base-layer BSD tests under `tests/base/bsd/`.
- Ensure each public base header can be included independently.
- Cover runtime `ring + nop` behavior where the host supports `io_uring`.
- Treat expected unavailable-host errors such as `-ENOSYS`, `-EPERM`, and
  `-EACCES` as covered error paths.
- Add compile coverage for new `prep_*` wrappers without submitting those SQEs
  to the kernel.
