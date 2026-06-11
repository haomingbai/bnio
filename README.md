# bupp

`bupp` is a small C++ wrapper around `liburing`.

## Dependencies

- C++20 compiler
- CMake 3.20 or newer
- `liburing >= 2.2`
- OpenSSL
- `pkg-config`
- `bexec`, resolved by CMake from a local checkout or from
  `https://github.com/haomingbai/bexec.git`

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

By default CMake fetches `bexec` from the `main` branch. To use a local checkout
instead:

```sh
cmake -S . -B build -DBUPP_BEXEC_SOURCE_DIR=/path/to/bexec
```

Use CMake's standard `BUILD_SHARED_LIBS` option to choose the library type:

```sh
cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON
cmake -S . -B build-static -DBUILD_SHARED_LIBS=OFF
```

## Examples

The `examples/base` directory builds several small executables covering probes,
NOP completions, timers, file I/O, poll readiness, provided buffers, and a small
echo server event loop.

## Async I/O Layer

`bupp::async_io` provides the low-level non-owning descriptor and buffer views.
The higher-level `bupp::io_context` layer provides sender factories, RAII TCP
objects, queued io_uring submission on Linux, and OpenSSL-backed SSL streams.

## Minimal Example

```cpp
#include <bupp/bupp.h>

int main() {
  bupp::base::ring ring;
  if (ring.queue_init(8) < 0) {
    return 1;
  }

  bupp::base::submission_queue_entry sqe = ring.get_sqe();
  if (sqe.raw() == nullptr) {
    return 1;
  }

  sqe.prep_nop();
  sqe.set_data64(42);

  if (ring.submit() < 0) {
    return 1;
  }

  bupp::base::completion_queue_entry cqe;
  if (ring.wait_cqe(cqe) < 0) {
    return 1;
  }

  const int result = cqe.res();
  ring.cqe_seen(cqe);
  return result;
}
```
