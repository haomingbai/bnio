# Build, Install & Packaging

This page covers building bnio, consuming it as a dependency, generating
coverage reports, and producing DEB/RPM packages. For the programming model
see [`usage/index.md`](usage/index.md); for runnable samples see
[`examples.md`](examples.md); for architecture see
[`design/architecture.md`](design/architecture.md).

## Dependencies

| Dependency    | Minimum Version | Notes                                |
| ------------- | --------------- | ------------------------------------ |
| C++ compiler  | C++20           | GCC 12+ or Clang 16+                 |
| CMake         | 3.20            |                                      |
| liburing      | 2.1             | Linux only; `pkg-config` required    |
| OpenSSL       | 1.1             | `pkg-config` required; TLS feature   |
| bexec         | 0.1.0           | Package, source tree, or FetchContent |

## Quick build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure
```

## Build options

| Option                         | Default      | Description                                  |
| ------------------------------ | ------------ | -------------------------------------------- |
| `BUILD_SHARED_LIBS`            | `OFF`        | Build `bnio` as a shared library             |
| `BNIO_BUILD_TESTS`             | top-level    | Build GoogleTest tests and enable CTest      |
| `BNIO_BUILD_EXAMPLES`          | top-level    | Build example executables                    |
| `BNIO_BUILD_BENCHMARKS`        | `OFF`        | Build bnio/Asio benchmark pairs and fetch Asio |
| `BNIO_BUILD_ASIO_EXAMPLES`     | `OFF`        | Build Asio examples and fetch Asio           |
| `BNIO_INSTALL`                 | top-level    | Generate installation and package files      |
| `BNIO_PACKAGE`                 | `OFF`        | Generate CPack DEB/RPM packaging rules       |
| `BNIO_ENABLE_COVERAGE`         | `OFF`        | Instrument GCC/Clang builds for coverage     |
| `BNIO_ENABLE_TSAN`             | `OFF`        | Build bnio targets with ThreadSanitizer      |
| `BNIO_GOOGLETEST_PROVIDER`     | `AUTO`       | `AUTO`, `FIND_PACKAGE`, `FETCH` (tests only)  |
| `BNIO_GOOGLETEST_GIT_TAG`      | `v1.17.0`    | Git ref used by the test-only `FETCH` provider |
| `BNIO_BEXEC_PROVIDER`          | `AUTO`       | `AUTO`, `FIND_PACKAGE`, `SOURCE`, `FETCH`     |
| `BNIO_BEXEC_SOURCE_DIR`        | empty        | Path used by the `SOURCE` provider            |
| `BNIO_BEXEC_GIT_TAG`           | `v0.1.0`     | Git ref used by the `FETCH` provider          |

When `bnio` is included with `add_subdirectory()` or `FetchContent`, tests,
examples, and installation rules default to off so they do not modify the
parent project's build.

### GoogleTest dependency

GoogleTest is resolved only after `BNIO_BUILD_TESTS` enables the `tests/`
subdirectory. With tests disabled, bnio does not call `find_package(GTest)`,
download GoogleTest, create GoogleTest targets, or expose it through the bnio
package.

The default `AUTO` provider accepts an existing `GTest::gtest_main` target,
tries `find_package(GTest)` (config and module modes), and then fetches the
pinned `BNIO_GOOGLETEST_GIT_TAG` as a fallback. The fetched copy does not build
GoogleMock and does not add GoogleTest installation rules.

```sh
# Require a preinstalled GoogleTest package; never download it.
cmake -S . -B build \
  -DBNIO_BUILD_TESTS=ON \
  -DBNIO_GOOGLETEST_PROVIDER=FIND_PACKAGE

# Skip all GoogleTest discovery and download work.
cmake -S . -B build-library -DBNIO_BUILD_TESTS=OFF
```

### bexec dependency providers

`bnio` always exposes the same `bnio::bnio` target, independently of how
`bexec` is supplied:

```sh
# Use an installed bexecConfig.cmake.
cmake -S . -B build \
  -DBNIO_BEXEC_PROVIDER=FIND_PACKAGE \
  -DCMAKE_PREFIX_PATH=/path/to/bexec

# Use a local bexec source checkout.
cmake -S . -B build \
  -DBNIO_BEXEC_PROVIDER=SOURCE \
  -DBNIO_BEXEC_SOURCE_DIR=/path/to/bexec

# Clone bexec during configuration.
cmake -S . -B build -DBNIO_BEXEC_PROVIDER=FETCH
```

`AUTO` first accepts an existing `bexec::bexec` target, then uses
`BNIO_BEXEC_SOURCE_DIR` when set, tries `find_package(bexec CONFIG)`, and
finally falls back to `FETCH`.

### Shared library build

```sh
cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON
cmake --build build-shared
```

Shared builds export the `BNIO_SHARED_LIBRARY` usage requirement, hide
non-public symbols, and install versioned library names with ABI version `0`.

## Consumption modes

All three consumption modes provide the `bnio::bnio` CMake target or equivalent
pkg-config link information.

### Installed CMake package

Install `bexec` first, then build and install `bnio` against that package:

```sh
cmake -S . -B build-install \
  -DCMAKE_BUILD_TYPE=Release \
  -DBNIO_BEXEC_PROVIDER=FIND_PACKAGE \
  -DCMAKE_PREFIX_PATH=/install/prefix
cmake --build build-install
cmake --install build-install --prefix /install/prefix
```

A CMake consumer can then use:

```cmake
find_package(bnio CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE bnio::bnio)
```

### pkg-config

The installation provides `bnio.pc`. On Linux it carries the required
`bexec`, OpenSSL, and liburing metadata:

```sh
c++ app.cpp $(pkg-config --cflags --libs bnio)
```

It can also be consumed as an imported CMake target:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(BNIO REQUIRED IMPORTED_TARGET bnio)
target_link_libraries(your_target PRIVATE PkgConfig::BNIO)
```

### Source tree

Direct source inclusion keeps the same namespaced target:

```cmake
add_subdirectory(path/to/bnio)
target_link_libraries(your_target PRIVATE bnio::bnio)
```

The parent may provide `bexec::bexec` first, select an installed package with
`BNIO_BEXEC_PROVIDER=FIND_PACKAGE`, point to a checkout with `SOURCE`, or allow
`bnio` to fetch it.

## Packaging (DEB / RPM)

Enable CPack packaging with `BNIO_PACKAGE=ON` (implies component-based
packaging via `cmake/cpack.cmake`):

```sh
cmake -S . -B build-pkg \
  -DCMAKE_BUILD_TYPE=Release \
  -DBNIO_INSTALL=ON \
  -DBNIO_PACKAGE=ON \
  -DBNIO_BEXEC_PROVIDER=FIND_PACKAGE \
  -DCMAKE_PREFIX_PATH=/path/to/bexec
cmake --build build-pkg
cd build-pkg && cpack -G DEB && cpack -G RPM
```

The package is split into runtime and development components:

| Generator | Runtime package | Development package            |
|-----------|-----------------|--------------------------------|
| DEB       | `libbnio0`      | `libbnio-dev` (depends on `libbnio0` and `bexec-dev`) |
| RPM       | `bnio`          | `bnio-devel` (requires `bnio` and `bexec-devel`) |

DEB runtime dependencies are resolved automatically via
`CPACK_DEBIAN_PACKAGE_SHLIBDEPS`. The development component pulls in the
matching runtime component and the bexec development package. RPM license is
set to MIT.

## Coverage report

Install `gcovr` 8.6 in a Python 3.10+ virtual environment, then configure an
instrumented build and run the tests:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install gcovr==8.6
cmake -S . -B build-coverage \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBNIO_BUILD_EXAMPLES=OFF \
  -DBNIO_ENABLE_COVERAGE=ON
cmake --build build-coverage
ctest --test-dir build-coverage --output-on-failure
cmake -E make_directory coverage
.venv/bin/gcovr --config gcovr.cfg build-coverage
```

On macOS, use AppleClang's gcov-compatible LLVM frontend for the final command:

```sh
.venv/bin/gcovr --config gcovr.cfg \
  --gcov-executable "xcrun llvm-cov gcov" build-coverage
```

The reports are written to `coverage/` as a text summary, detailed HTML, and
Cobertura XML. The GitHub Actions matrix uploads a separate artifact for the
Linux/io_uring and macOS/kqueue runs.
