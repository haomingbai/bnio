# Architecture

`bnio` is organized into three abstraction layers. Each layer has a distinct
responsibility, and layers depend strictly downward: higher layers use lower
layers; lower layers never reference higher ones.

Both native backends are implemented: Linux uses `io_uring`
(one-thread-one-uring model), and macOS/BSD use `kqueue`. The full
`io_context`, TCP, UDP, local-socket, DNS, timer, and TLS APIs are shared
across the two backends; the kqueue backend is covered by the repository's
test suites and the macOS CI job (the io_uring backend runs on the Linux CI
job).

## Topics

- [Layer overview, ownership, and platform backends](architecture/layers.md)
- [Layer 1: `bnio::base`](architecture/base-layer.md)
- [Layer 2: `bnio::async_io`](architecture/async-io-layer.md)
- [Layer 3: `bnio::io_context`](architecture/io-context-layer.md)
- [Worker scheduling: suspend list, directed wakeup, non-atomic local queue](architecture/worker-scheduling.md)
- [Header dependency graph and namespace map](architecture/header-namespace-map.md)
- [Local (AF_UNIX) socket type family split](architecture/local-socket-split.md)
