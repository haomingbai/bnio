# Architecture

`bupp` is organized into three abstraction layers. Each layer has a distinct
responsibility, and layers depend strictly downward: higher layers use lower
layers; lower layers never reference higher ones.

The primary platform is Linux with `io_uring` (one-thread-one-uring model).
BSD `kqueue` base wrappers are implemented; the full `kqueue` backend is in
progress.

## Topics

- [Layer overview, ownership, and platform backends](architecture/layers.md)
- [Layer 1: `bupp::base`](architecture/base-layer.md)
- [Layer 2: `bupp::async_io`](architecture/async-io-layer.md)
- [Layer 3: `bupp::io_context`](architecture/io-context-layer.md)
- [Header dependency graph and namespace map](architecture/header-namespace-map.md)
