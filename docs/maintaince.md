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

- `bupp::base` is a thin C API to C++ object mapping.
- On Linux, public wrapper headers declare API; method implementations live under
  `src/base/linux/` and are compiled into `bupp`.
- On BSD, wrapper headers live in `include/bupp/base/bsd/` and implementations
  in `src/base/bsd/`.
- Keep system call return semantics: successful values are non-negative and
  failures are negative `errno` values.
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
- On Linux, add declarations to `include/bupp/base/linux/` and definitions to
  `src/base/linux/`.
- On BSD, add declarations to `include/bupp/base/bsd/` and definitions to
  `src/base/bsd/`.
- Keep `bupp` buildable as both a static and shared library with
  `BUILD_SHARED_LIBS`.

## Doxygen

- Every public method in a wrapper header should have a Doxygen comment.
- Each wrapper method should include an `@see` reference to the corresponding C
  API or C structure.
- Run `scripts/check-doc.sh` before submitting documentation changes.

## Tests

- Write runtime tests as focused GoogleTest `TEST` cases and register them with
  the shared `bupp_add_gtest` CMake helper.
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
