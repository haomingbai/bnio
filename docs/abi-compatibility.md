# bnio ABI Compatibility

## Scope

This policy covers the Linux x86_64 shared-library packages built by the release
workflow: glibc systems, io_uring backend, `BUILD_SHARED_LIBS=ON`, and hidden
symbol visibility. macOS/BSD kqueue builds and musl builds are source-build
targets and are outside the packaged ABI baseline described here.

## Policy

**Same-version builds are ABI compatible by default.** Two artifacts built from
the same bnio source version are ABI compatible when they use the same
dependency ABI set:

- the same bexec version — bexec is header-only, so its code is compiled into
  every consumer translation unit;
- the same OpenSSL ABI — `libssl.so.3` / `OPENSSL_3.0.0` and `libssl.so.4` /
  `OPENSSL_4.0.0` are different ABI units;
- the same liburing ABI — the library must export `io_uring_enable_rings`
  (liburing 2.5 and newer; see the dependency table);
- a compatible C++ runtime and glibc floor, as recorded by the artifact's
  `readelf --version-info` output.

ABI compatibility is therefore primarily a property of the dependency set, not
of the bnio version number alone. The dependency floors in this document were
verified across the tested distribution matrix: builds made on the Debian 12
and Ubuntu 24.04 baselines installed, linked, and ran consumers on the
supported targets listed below without rebuilding on the target.

**Different bnio versions are not ABI compatible by default.** Before `1.0.0`,
any version change may alter exported symbols, class layouts, and inline or
template behavior. Rebuild and re-verify every dependent library and
application when changing the bnio version. The SONAME is `libbnio.so.0`; it
does not encode the minor or patch version, so two bnio versions cannot be
installed side by side. An unchanged SONAME is **not** an ABI promise.

**From `1.0.0` onward, SemVer applies.** Incompatible ABI changes require a
major-version bump and a SONAME major-version bump.

### Relationship to CMake `SameMajorVersion`

`bnioConfigVersion.cmake` is generated with `COMPATIBILITY SameMajorVersion`.
In the `0.x` series every release has major version `0`, so a request such as
`find_package(bnio <version>)` is accepted by any later `0.y` package. That
acceptance only means the CMake package and its target definitions were found;
it is not an ABI promise and does not inspect exported symbols, layouts, or
compiler-generated code.

Consumers that need a specific build should request an exact version
(`find_package(bnio <version> EXACT)`) or build bnio and its dependencies from
source. CMake configure-time acceptance must not be read as permission to swap
versions in a deployed binary.

## ABI unit: bnio + bexec + binary dependencies

bnio does not have a self-contained ABI. Its public headers expose types from
the header-only **bexec** library directly in the public API:

- execution concepts — `bexec::scheduler`, `bexec::sender` — constrain the
  public customization-point interfaces;
- receiver CPOs — `bexec::set_value`, `bexec::set_stopped`, `bexec::get_env`,
  `bexec::get_stop_token` — appear in the signatures of the async operation
  states that consumers instantiate;
- consumers drive every bnio operation through `bexec::connect` / `bexec::start`.

Because bexec is header-only, all of its code is compiled into the consumer's
translation units. The ABI unit is therefore:

> **bnio version + exact bexec version + binary dependency ABI set**

Pin the exact bexec version used to build bnio; any bexec change — including a
patch release — is an ABI change and requires rebuilding and re-verifying bnio
and its consumers. The build system pins the version used by the release
(`cmake/bexec.cmake`), enforces a lower bound for consumers, and does not enforce
an upper bound; consumers must pin it themselves.

bexec is not shipped by the Debian/Ubuntu, Fedora, openSUSE, or Arch
repositories. The bnio development packages declare a `bexec` dependency, so
consumers must install bexec from source first.

## Dependency requirements

| Dependency | Requirement | Notes |
|---|---|---|
| bexec | Exact version pinned by `cmake/bexec.cmake`; any change is an ABI change | Header-only; compiled into every consumer |
| OpenSSL | Release packages use the OpenSSL 3 ABI: `libssl.so.3`, `OPENSSL_3.0.0` symbols | OpenSSL 4 (`libssl.so.4`) is a separate ABI unit. Build against the oldest OpenSSL ABI you intend to support. Fedora 45 provides OpenSSL 3 compatibility libraries as `openssl3-libs`. |
| liburing | A build that exports `io_uring_enable_rings` (2.5 or newer; RPMs require `liburing.so.2(LIBURING_2.4)`) | liburing 2.1 and 2.3 declare the header but do not export the symbol. Debian 12's stock liburing 2.3 and the liburing 2.1 shipped by Ubuntu 22.04/openSUSE Leap 15.6 cannot run the library without an upgrade. The current CMake lower bound (`liburing>=2.1`) is weaker than this effective requirement. |
| glibc / libstdc++ | `GLIBC_2.34`, `GLIBCXX_3.4.21` runtime floor for the reference build | These are floors recorded from the artifact, not guarantees for older runtimes. |
| Compiler (consumer) | C++20; GCC 12 or newer recommended | GCC 11.5 (the default on EL9, Amazon Linux 2023, and Ubuntu 22.04) cannot compile the pinned bexec headers; use gcc-toolset. openSUSE Leap 15.6's default GCC 7.5 is too old and needs gcc13. clang 18 and newer build the library and examples; one integration test currently requires `-Wno-c++11-narrowing`. |
| libc | glibc only | Alpine/musl is not covered by the DEB/RPM artifacts; build from source with the musl toolchain. |

## Inspecting a build

The ABI unit is the built artifact, not a fixed symbol count or BuildID. For a
given `libbnio.so`:

```sh
readelf -d libbnio.so | grep -E 'SONAME|NEEDED'
readelf --version-info libbnio.so | grep -oE 'GLIBC_?[A-Z_]*_[0-9.]+' | sort -Vu | tail
nm -D --defined-only libbnio.so | wc -l
ldd -r libbnio.so
```

Record the SONAME, NEEDED entries, required symbol versions, and exported symbol
count in the release metadata; do not compare against a hard-coded number from
an older report. `ldd -r` must report no missing symbols on a supported target.

## Release artifacts and signing

The release workflow produces two components per package format:

| Format | Runtime package | Development package | File-name pattern |
|---|---|---|---|
| DEB | `libbnio0` | `libbnio-dev` | `bnio-<version>-Linux-{runtime,development}.deb` |
| RPM | `bnio` | `bnio-devel` | `bnio-<version>-Linux-{runtime,development}.rpm` |

`SHA256SUMS.deb` is attached to each release for DEB integrity verification.

Tagged releases sign the RPM packages. The workflow fails closed if the signing
key is not configured, and attaches the public key
(`bnio-rpm-signing-key.asc`) and `RPM-SIGNING.txt` to the release. Verify a
download with:

```sh
rpm --import bnio-rpm-signing-key.asc
rpmkeys --checksig bnio-<version>-Linux-runtime.rpm
```

DEB packages are not individually signed: apt does not verify per-package
signatures, and the project publishes release files rather than an apt
repository. Verify the published SHA-256 checksums. If an apt repository is
introduced later, its `InRelease` metadata must be signed. See
[`release-signing.md`](release-signing.md) for the key setup and rotation
procedure.

The release workflow also checks that the pushed tag matches `BNIO_VERSION`
before building and signing, so package versions cannot silently diverge from
the tag.

## Verification scope and limits

The dependency baseline was verified with Release builds made on the Debian 12
and Ubuntu 24.04 baselines (OpenSSL 3.0, liburing 2.3/2.5) and installed across
a glibc-only matrix: Ubuntu 24.04 and 26.04, Debian 13, Fedora 43/44/45, Rocky
Linux 10, and openSUSE Leap 16.1. The Debian 12 baseline covered the Debian/
Ubuntu targets; the Ubuntu 24.04 baseline covered the RPM targets. The packages
installed, linked against the distribution's native toolchain, and ran timer
and TCP loopback consumers on every supported target.

Known limits, all dependency-driven rather than bnio-version-specific:

- Debian 12 (stock liburing 2.3), openSUSE Leap 15.6, and Ubuntu 22.04 (liburing
  2.1) lack the required `io_uring_enable_rings` export. The library cannot load
  there without upgrading liburing.
- EL9 and Amazon Linux 2023 satisfy the glibc floor and can run the library, but
  their default GCC 11.5 cannot compile the pinned bexec headers; use
  gcc-toolset-14 or newer for consumers. openSUSE Leap 15.6 also needs gcc13.
- Alpine/musl is not covered by the glibc-linked packages.

Earlier version-specific reports remain available in git history. This document
is the current policy and deliberately does not pin itself to a bnio release
version; the version being released is identified by the tag, `BNIO_VERSION`,
and the packaged artifact itself.

## Recommendation: build from source

The most reliable way to consume bnio at this stage is to compile it together
with the pinned bexec version in your own build — via CMake `FetchContent` /
`add_subdirectory`, or a source install of both — using the same dependency ABI
set described above. Prebuilt DEB/RPM artifacts are provided for the tested
glibc targets and carry no cross-version ABI guarantee before `1.0.0`. Whenever
bnio or bexec changes, rebuild all dependent code.
