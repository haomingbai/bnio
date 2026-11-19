# bnio v0.4 ABI Compatibility Report

- **Release:** bnio v0.4 (package version 0.4.0)
- **Library:** `libbnio.so.0.4.0` (SONAME `libbnio.so.0`, development symlink `libbnio.so`)
- **Scope:** Linux x86_64, glibc systems, io_uring backend, shared-library build
  (`BUILD_SHARED_LIBS=ON`, hidden symbol visibility)
- **Supersedes:** the [bnio v0.2 report](https://github.com/haomingbai/bnio/blob/v0.2.0/docs/abi-compatibility.md), which stays in git history

## Overview

bnio is a C++20 asynchronous I/O library. Every operation is a lazy sender that
composes with the standard receiver pattern; the Linux backend runs on
io_uring, and macOS/BSD use kqueue.

The v0.4 packages are built by the repository release workflow
(`.github/workflows/release.yml`) from the `v0.4.0` tag. The source tree now
defaults to `BNIO_VERSION=0.4.0`; the version can still be overridden at
configure time with `-DBNIO_VERSION=...`. The release job builds a Release
shared library with `BUILD_SHARED_LIBS=ON` against **bexec v0.1.1** and
generates **DEB and RPM packages only**. It does not produce TGZ archives or
distribution packages beyond those two formats.

## ABI compatibility policy

**Patch releases within one minor series are ABI compatible.** For every
`0.4.x` release (compatibility granularity `0.0.1`), the exported symbol set,
class layouts, and inline/template behavior are preserved. A consumer built
against one `0.4.x` package may load another `0.4.y` package without rebuilding.

**Different minor releases are not ABI compatible.** `0.3` → `0.4` may change
exported symbols, class layouts, and inline/template behavior. Rebuild every
dependent library and application when crossing a minor version, and do not
install two minor versions side by side: their SONAMEs are both
`libbnio.so.0`, so the loader cannot distinguish them. An unchanged SONAME is
therefore **not** an ABI promise at this stage.

**Major releases follow SemVer.** From `1.0.0` onward, incompatible ABI changes
require a major-version bump and a SONAME major-version bump.

### Relationship to CMake `SameMajorVersion`

`bnioConfigVersion.cmake` is generated with
`COMPATIBILITY SameMajorVersion`. That rule checks that the package has the
same major version and is at least the requested version. In the `0.x` series
every release has major version `0`, so a request such as
`find_package(bnio 0.4)` is accepted by `0.4`, `0.5`, or any later `0.y`
package, regardless of ABI. That acceptance means only that the CMake package
and its target definitions were found. It is not an ABI promise and does not
inspect exported symbols, layouts, or compiler-generated code.

Consumers that need a specific build should request an exact version
(`find_package(bnio 0.4.0 EXACT)`) or build bnio and its dependencies from
source. CMake configure-time acceptance must not be read as permission to swap
minor versions in a deployed binary.

## Coupling with bexec v0.1.x (read first)

**bnio v0.4 does not have a self-contained ABI.** Its public headers expose
types from the header-only library **bexec** directly in the public API:

- execution concepts — `bexec::scheduler`, `bexec::sender` — constrain the
  public customization-point interfaces (e.g. `include/bnio/io_context_cpo/concepts.h`);
- receiver CPOs — `bexec::set_value`, `bexec::set_stopped`, `bexec::get_env`,
  `bexec::get_stop_token` — appear in the signatures of the async operation
  states that consumers instantiate;
- consumers drive every bnio operation through `bexec::connect` / `bexec::start`.

Because bexec is header-only, all of its code is compiled into the consumer's
own translation units. Layout, ODR, and template-instantiation compatibility of
any bnio consumer therefore depends on the **exact bexec version** in use, not
only on the bnio binary.

The bnio v0.4 ABI is therefore defined as a single fixed combination:

> **bexec v0.1.1 + bnio v0.4.0** (release packages)

Compatibility is guaranteed only for this fixed combination: both the bexec and
bnio versions must be pinned. Because bexec's header-only code is compiled into
every consumer, any change of the bexec version — even a patch release within
v0.1.x — requires rebuilding and re-verifying bnio and its consumers. bnio
patch releases within 0.4.x remain ABI compatible as described above, provided
the bexec version is unchanged.

Compatibility rules for consumers:

- The documented dependency is **bexec >= 0.1.0**, and bnio v0.4 is compatible
  only with the **v0.1.x** line. Any change of bexec — including a patch
  release — is an ABI change of bnio, even if the bnio binary is untouched.
- The build system enforces the lower bound: `BNIO_BEXEC_MIN_VERSION` is
  `0.1.0`, the FETCH provider defaults to tag `v0.1.1`, and the generated
  package dependencies request `bexec >= 0.1.0`. It does **not** enforce the
  upper bound, so consumers must avoid moving to a different bexec version
  without rebuilding and re-verifying bnio.
- The v0.4 DEB/RPM release packages are built against **bexec v0.1.1**, so
  consumers of those prebuilt packages must compile against bexec v0.1.1 as
  well. Using any other bexec version locally — including v0.1.0 — requires
  rebuilding bnio and re-verifying it; it cannot be assumed ABI compatible
  with the prebuilt packages.
- bexec ships no binary package in the Debian/Ubuntu, Fedora, openSUSE or Arch
  repositories. The bnio development packages declare the dependency
  (`libbnio-dev` → `bexec`, `bnio-devel` → `bexec`), so bexec has to be
  installed from source first.

## Dependency requirements

| Dependency | Requirement | Enforced by the build system |
|---|---|---|
| bexec | >= 0.1.0, compatible within v0.1.x | Lower bound **yes** (`BNIO_BEXEC_MIN_VERSION=0.1.0`, packages require `bexec >= 0.1.0`); upper bound `v0.1.x` **no**. FETCH default tag `v0.1.1`; release packages built against `v0.1.1` |
| OpenSSL | >= 1.1.1 (documented requirement) | **No** — `find_package(OpenSSL REQUIRED)` has no version bound |
| liburing | >= 2.1 (Linux backend) | **Yes** — `pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.1)` |
| Compiler | C++20 | Yes — `target_compile_features(bnio PUBLIC cxx_std_20)` |

## Binary identity

The following data was collected from a **v0.4.0 reference build** made from the
current tree with `-DBNIO_VERSION=0.4.0 -DBUILD_SHARED_LIBS=ON`, not from a
published package. The release CI rebuilds and repackages the library, so the
BuildID and the exact NEEDED list of the published artifacts will differ.

| Property | Value |
|---|---|
| File name | `libbnio.so.0.4.0` |
| **SONAME** | **`libbnio.so.0`** |
| Exported (dynamic, defined) symbols | 456 |
| BuildID | `17d1e42fc26d7eec0b56bb06acee21f1715da321` |
| Reference build environment | Fedora Linux, GCC 16.2.1, CMake 4.3.0, OpenSSL 4.0.3, liburing 2.13 |

The release packages are built on Ubuntu in CI against the distribution's
OpenSSL and liburing. For the symbol count and BuildID of a published package,
inspect the uploaded CI artifact rather than this reference build.

## Release artifacts and verification

The release workflow produces exactly two components per package format:

| Format | Runtime package | Development package | File-name pattern |
|---|---|---|---|
| DEB | `libbnio0` | `libbnio-dev` | `bnio-<version>-Linux-{runtime,development}.deb` |
| RPM | `bnio` | `bnio-devel` | `bnio-<version>-Linux-{runtime,development}.rpm` |

No TGZ artifact is produced by the current workflow. The earlier v0.2 report's
TGZ-based Arch Linux and Alpine rows are therefore historical and are not
reproduced here as a support claim.

### CI test job

The release workflow's test job runs on Ubuntu with `CMAKE_BUILD_TYPE=Debug`,
`BUILD_SHARED_LIBS=ON`, and examples disabled. It runs the non-stress suite,
then repeats the stress suite with `--repeat until-fail:20`.

### Packaged-binary smoke matrix (historical v0.2 data)

The table below is retained from the **v0.2 report**. The data was collected on
v0.2 DEB/RPM artifacts and has **not been re-collected for v0.4** at the time of
writing. It documents the verification method and the distributions that were
covered; it is not a v0.4 compatibility claim. A v0.4 matrix must be collected
from the v0.4 packages with the same method; this report does not invent one.

To re-run the method against v0.4 packages, use the current example targets
for the functional checks: `timer_chain`, `dns_lookup`, `poll_fd`,
`echo_server` (including SIGINT graceful shutdown), `file_cat`, and
`mini_curl`. Per distribution, the procedure is:

1. Start a clean container from the official image.
2. Install the distribution's native toolchain, liburing development package,
   and OpenSSL development package.
3. Install bexec v0.1.x from source.
4. Install the bnio DEB/RPM with the native package manager.
5. Compile the example programs against the installed headers and shared
   library.
6. Smoke-run the functional checks listed above.

The PASS counts in the table come from the v0.2 run and used the checks
available at that tag; they are not a v0.4 result.

| Distribution | glibc | g++ | CMake | liburing | Result (v0.2 data) |
|---|---|---|---|---|---|
| Ubuntu 24.04.4 LTS | 2.39 | 13.3.0 | 3.28.3 | 2.5 | 6/6 PASS |
| Ubuntu 26.04 | 2.43 | 15.2.0 | 4.2.3 | 2.14 | 6/6 PASS |
| Debian 13 (trixie) | 2.41 | 14.2.0 | 3.31.6 | 2.9 | 6/6 PASS |
| Fedora 43 | 2.42 | 15.3.1 | 3.31.11 | 2.9 | 6/6 PASS |
| Rocky Linux 10.2 | 2.39 | 14.3.1 | 3.31.8 | 2.12 (CRB) | 6/6 PASS |
| openSUSE Leap 16.0 | 2.40 | 15.2.0 | 3.31.7 | 2.8 | 6/6 PASS |

The v0.2 matrix spanned GCC 13.3 → 16.2.1, glibc 2.39 → 2.44, CMake 3.28 →
4.4.3, and liburing 2.5 → 2.15 against a single glibc-linked binary. Those
numbers remain useful as a description of the tested range, but they were
measured on v0.2 artifacts.

### musl / Alpine

Alpine uses musl libc and is not a supported target for the glibc-linked DEB
and RPM packages. musl consumers must build bnio from source with their own
toolchain; that configuration is outside the ABI baseline defined here.

## Recommendation: build from source

The most reliable way to consume bnio at this stage is to compile bnio v0.4.0
together with bexec v0.1.x as part of your own build — via CMake
`FetchContent` / `add_subdirectory`, or a source install of both — with your own
toolchain. The prebuilt DEB/RPM artifacts are provided for evaluation on the
distributions listed above: they carry no ABI stability guarantee across bnio
releases. Whenever bnio or bexec is upgraded, rebuild all dependent code.
