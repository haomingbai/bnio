# bnio

**bnio** is a C++20 async I/O library. Every operation is a lazy sender that
composes with the standard receiver pattern — TCP, TLS (OpenSSL), UDP, DNS
resolution, timers, and composed writes are all built in.

> **Status:** Linux runs on io_uring, macOS and BSD on kqueue. Both backends
> share the same `io_context`, TCP, UDP, DNS, timer, and TLS APIs. TLS needs
> OpenSSL ≥ 1.1.

---

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

A minimal example — a one-shot timer, the same on either backend:

```cpp
#include <bnio/bnio.h>
#include <bexec/bexec.hpp>
#include <chrono>

struct timer_receiver {
  void set_value() noexcept { /* timer expired */ }
  void set_error(std::error_code) noexcept {}
  void set_stopped() noexcept {}
};

int main() {
  bnio::io_context ctx;
  bnio::steady_timer timer(ctx, std::chrono::milliseconds(100));

  auto sender = timer.async_wait();
  auto op = bexec::connect(std::move(sender), timer_receiver{});
  bexec::start(op);

  ctx.run();  // returns once the timer completes
}
```

Each async factory returns a lazy sender; `bexec::connect` binds it to a
receiver, `bexec::start` submits it, and `ctx.run()` pumps the loop until it
finishes. The [usage guide](docs/usage/index.md) covers the sender/receiver
model in depth, and [examples](docs/examples.md) has TCP, TLS, DNS, and HTTP
samples (including a small `mini_curl`).

---

## Using bnio

The simplest option is to add bnio straight to your CMake tree:

```cmake
add_subdirectory(bnio)
target_link_libraries(your_app PRIVATE bnio::bnio)
```

Prefer to install it first? bnio ships a standard CMake package and a
pkg-config file:

```sh
cmake --install build --prefix /usr/local
```

```cmake
find_package(bnio CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE bnio::bnio)
```

```sh
c++ app.cpp $(pkg-config --cflags --libs bnio)
```

bnio also builds DEB and RPM packages through CPack. Enable it with
`BNIO_PACKAGE=ON` and run `cpack`:

```sh
cmake -S . -B build-pkg -DCMAKE_BUILD_TYPE=Release -DBNIO_INSTALL=ON -DBNIO_PACKAGE=ON
cmake --build build-pkg
(cd build-pkg && cpack -G DEB && cpack -G RPM)
```

That gives you a runtime and a development package for each format —
`libbnio0` / `libbnio-dev` on Debian-style systems, `bnio` / `bnio-devel` on
RPM-based ones.

Build options, dependency providers, coverage, and packaging details are in
[docs/build.md](docs/build.md).

---

## License

MIT — see [LICENSE](LICENSE).
