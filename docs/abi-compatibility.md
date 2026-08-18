# bnio v0.1 ABI Compatibility Report

- **Release:** bnio v0.1 (package version 0.1.0)
- **Library:** `libbnio.so.0.1.0` (SONAME `libbnio.so.0`, development symlink `libbnio.so`)
- **Report date:** 2026-08-17
- **Scope:** Linux x86_64, glibc systems, io_uring backend, shared-library build
  (`BUILD_SHARED_LIBS=ON`, hidden symbol visibility)

## Overview

bnio is a C++20 asynchronous I/O library. Every operation is a lazy sender that
composes with the standard receiver pattern; the Linux backend runs on io_uring.
The v0.1 binary artifacts (DEB/RPM/TGZ) were built from bnio git HEAD
`ee5c8fa9d2a9431384e9840da9c3e7db7de40ea9` ("fix: unblock run loop when stop()
arrives via signal handler") in an `ubuntu:24.04` container with GCC 13.3.0,
CMake 3.28.3, Ninja 1.11.1, OpenSSL 3.0.13, and liburing 2.5.

This report defines the ABI compatibility baseline for bnio v0.1 and records the
distribution matrix on which the released packages were verified.

## ABI stability

**The bnio ABI is currently not stable.** v0.1 is the first published ABI
baseline, and no backward-compatibility guarantee is made across releases:

- Exported symbols, class layouts, and inline/template behavior may change in
  any future release without SONAME preservation.
- The only configuration covered by this report is the exact unit
  **bexec v0.1.0 + bnio v0.1.0** (see the coupling section below); any other
  combination is unverified.

**Recommendation: build from source.** The most reliable way to consume bnio at
this stage is to compile bnio v0.1.0 together with bexec v0.1.0 as part of your
own build — via CMake `FetchContent` / `add_subdirectory`, or a source install
of both — with your own toolchain. The prebuilt DEB/RPM/TGZ artifacts are
provided for evaluation on the verified distributions listed in this report:
they are forward-compatible across the tested glibc/toolchain range, but they
do not imply ABI stability across bnio releases. Whenever bnio (or bexec) is
upgraded, rebuild all dependent code.

## Coupling with bexec v0.1 (read first)

**bnio v0.1 does not have a self-contained ABI.** Its public headers expose
types from the header-only library **bexec v0.1** directly in the public API:

- execution concepts — `bexec::scheduler`, `bexec::sender` — constrain the
  public customization-point interfaces (e.g. `include/bnio/io_context_cpo/concepts.h`);
- receiver CPOs — `bexec::set_value`, `bexec::set_stopped`, `bexec::get_env`,
  `bexec::get_stop_token` — appear in the signatures of the async operation
  states that consumers instantiate;
- consumers drive every bnio operation through `bexec::connect` / `bexec::start`.

Because bexec is header-only, all bexec code is compiled into the consumer's own
translation units. Layout, ODR, and template-instantiation compatibility of any
bnio consumer therefore depends on the **exact bexec version** in use, not only
on the bnio binary.

Consequently, the bnio v0.1 ABI is defined as a single unit:

> **bexec v0.1.0 (commit `ddd9e9c5ec586ac8d6f221d6f3bce0e8fefd8ccd`) + bnio v0.1.0**
> (built at `ee5c8fa`).

Compatibility rules for consumers:

- Consumers **must** build against bexec **v0.1.x**. The released packages were
  built and verified exclusively against bexec v0.1.0 (tag `v0.1.0`, commit
  `ddd9e9c5`, installed from source to `/usr/local`, consumed via
  `BNIO_BEXEC_PROVIDER=FIND_PACKAGE`).
- Any **major or minor version change of bexec constitutes an ABI change of
  bnio**, even if the bnio binary (`libbnio.so.0.1.0`) is unchanged. Do not
  upgrade bexec past v0.1.x without rebuilding/re-verifying bnio.
- bexec has no binary package in current Debian/Ubuntu or Fedora repositories;
  the development packages declare the dependency (`libbnio-dev` → `bexec`,
  `bnio-devel` → `bexec`) but it must be satisfied by installing bexec from
  source before installing the bnio development package.

## Test methodology (2026-08-17)

The released packages were verified on a 7-distribution container matrix
(`sudo docker`, `--security-opt seccomp=unconfined` to permit io_uring,
`--network=host`). Distribution selection was fixed on baseline date
**2026-02-17** as the then-current LTS/release of each major family (see
`.artifacts/distro-matrix.md`); Rocky Linux 10 serves as the RHEL-10-family
representative (ABI-identical to RHEL 10 / CentOS Stream 10 / AlmaLinux 10).

Per distribution, the procedure was:

1. Start a clean container from the official image.
2. Install the distribution's **native toolchain** (g++, cmake, liburing
   development package, OpenSSL development package).
3. Install bexec v0.1.0 from source to `/usr/local`.
4. Install the released bnio package through the native package manager
   (DEB/RPM), or unpack the TGZ onto `/usr` where no native format applies.
5. Compile the example programs from the repository `examples/` tree against
   the **installed** bnio headers and shared library, using the distribution's
   native toolchain — i.e. the compiler and system headers come from the target
   distribution, while bnio itself comes from the released binary.
6. Smoke-run six functional checks:
   - `timer_chain` — chained steady-timer waits;
   - `dns_lookup` — localhost name resolution;
   - `poll_fd` — descriptor polling;
   - `echo_server` — TCP echo round-trip;
   - `echo_server_sigint` — clean exit within 5 s of SIGINT (validates the
     `ee5c8fa` stop-via-signal-handler fix);
   - `mini_curl` — HTTP fetch against a local server.

`tcp_client` and `udp_echo` were excluded from the matrix: at matrix time they
carried a known example-level operation-lifetime use-after-free
(`.artifacts/example-check/REPORT.md`) that is unrelated to ABI compatibility.
Both examples have since been fixed (along with an `echo_server` shutdown
state-machine bug); see `.artifacts/example-check/FIX-VERIFICATION.md`. The
fixes are example-only — the library binary under test is unchanged.

## Compatibility matrix

| Distribution | glibc | g++ | CMake | liburing | Package installation | Result |
|---|---|---|---|---|---|---|
| Ubuntu 24.04.4 LTS | 2.39 (2.39-0ubuntu8.8) | 13.3.0 | 3.28.3 | 2.5 | `apt` runtime.deb + `dpkg --force-depends` dev.deb | **6/6 PASS** |
| Debian 13 (trixie) | 2.41 (2.41-12+deb13u3) | 14.2.0 | 3.31.6 | 2.9 | same as Ubuntu | **6/6 PASS** |
| Fedora 43 | 2.42 | 15.3.1 | 3.31.11 | 2.9 | `dnf` runtime.rpm + `rpm --nodeps` dev.rpm | **6/6 PASS** |
| Rocky Linux 10.2 | 2.39 | 14.3.1 | 3.31.8 | 2.12 (CRB) | same as Fedora | **6/6 PASS** |
| openSUSE Leap 16.0 | 2.40 | 15.2.0 | 3.31.7 | 2.8 | `zypper` runtime.rpm + `rpm --nodeps --replacefiles` dev.rpm | **6/6 PASS** |
| Arch Linux (rolling) | 2.44 | 16.2.1 | 4.4.2 | 2.15 | tar.gz unpacked onto `/usr` | **6/6 PASS** |
| Alpine 3.23 (musl) | musl (no glibc) | — | 4.1.3 | 2.12 | tar.gz onto `/usr` | **Expected failure** (see below) |

All six glibc distributions pass every check, including the SIGINT
graceful-shutdown test (`SUMMARY: pass=6 fail=0` per distribution; orchestrator
exit codes: ubuntu/debian/fedora/rocky/leap/arch `rc=0`, alpine `rc=1`).
The matrix spans GCC 13.3 → 16.2, glibc 2.39 → 2.44, CMake 3.28 → 4.4, and
liburing 2.5 → 2.15 against the single bnio v0.1 binary built with GCC 13.3 on
glibc 2.39 — the released artifacts are forward-compatible across this range.

## musl / Alpine (expected failure)

Alpine 3.23 uses musl libc and is **not** a supported target for the released
glibc-linked binaries. The Alpine run exists to verify that conclusion itself
and does not count toward the compatibility pass rate:

1. **Library loading fails:** `ldd /usr/lib/libbnio.so.0.1.0` reports
   `Error loading shared library ld-linux-x86-64.so.2: No such file or
   directory (needed by /usr/lib/libbnio.so.0.1.0)` — a glibc-linked shared
   object cannot be resolved by the musl loader.
2. **Example compilation fails:** `/usr/include/liburing.h:15:10: fatal error:
   linux/swab.h: No such file or directory` — the musl + kernel-headers
   ecosystem differs from glibc's.
3. **Conclusion:** the DEB/RPM/TGZ artifacts of bnio v0.1 are applicable to
   glibc-based distributions only. musl-based consumers must build bnio from
   source on their own toolchain; such a configuration is outside the ABI
   baseline defined in this report.

## Known packaging notes

These are packaging-metadata observations from the matrix run; none affects
runtime functionality:

- **DEB development package requires `--force-depends`:** `libbnio-dev`
  declares a dependency on `bexec`, which is installed from source and
  therefore invisible to `dpkg`. Install bexec (≥ v0.1.0) first, then the dev
  package with `dpkg --force-depends`.
- **RPM development package requires `--nodeps`:** same cause as above —
  `bnio-devel` requires `bexec`, which has no RPM providing it.
- **openSUSE Leap additionally requires `--replacefiles`:** CPack's
  `/usr/lib/pkgconfig` directory entry in the RPM conflicts with the
  distribution's `filesystem` package.
- **Rocky Linux:** `liburing-devel` lives in the CRB repository; enable it with
  `--enablerepo=crb`.
- **Missing execute bit on the `.so` in RPM/TGZ artifacts:** on
  Fedora/Rocky/Leap/Arch, `ldd` emits `warning: you do not have execution
  permission for '/usr/lib/libbnio.so.0.1.0'`. Dynamic libraries do not need
  the execute bit to be mapped by `ld.so`; all tests pass. Recorded as a
  packaging polish item for a future release (DEB artifacts are unaffected).

## Raw data and audit trail

- Raw matrix data: `.artifacts/matrix-test/RAW-DATA.md`
- Per-distribution full logs: `.artifacts/matrix-test/logs/<distro>.log`
  (orchestrator summary: `logs/_summary.txt`)
- Distribution selection rationale: `.artifacts/distro-matrix.md`
- Package build manifest and checksums: `.artifacts/packages/PACKAGING-NOTES.md`

**Note:** `.artifacts/` is not committed to the repository; the paths above are
retained as an audit trail for the release process, not as in-tree
documentation.
