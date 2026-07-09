# Layer Overview

```mermaid
graph TB
    subgraph L3["Layer 3 — bupp::io_context"]
        C3_ctx["io_context<br/>(event loop + scheduler factory)"]
        C3_tcp["tcp_socket / tcp_acceptor<br/>(RAII fd owners)"]
        C3_ssl["ssl_context / ssl_stream<br/>(RAII SSL owners)"]
        C3_buf["mutable_buffer / const_buffer<br/>dynamic_string_buffer<br/>(non-owning views / adapters)"]
    end

    subgraph L2["Layer 2 — bupp::async_io"]
        C2_buf["buffer_view<br/>(non-owning)"]
        C2_desc["descriptor_view<br/>(non-owning)"]
        C2_stream["stream_socket_view<br/>(non-owning)"]
        C2_listen["listening_socket_view<br/>(non-owning)"]
        C2_ip["address / endpoint<br/>(value types)"]
        C2_native["linux_native::io_uring_*<br/>(platform operations)"]
    end

    subgraph L1["Layer 1 — bupp::base"]
        C1_ring["ring<br/>(RAII io_uring owner)"]
        C1_sqe["submission_queue_entry<br/>(non-owning SQE view)"]
        C1_cqe["completion_queue_entry<br/>(non-owning CQE view)"]
        C1_probe["probe / params<br/>(RAII / value types)"]
    end

    subgraph SYS["System"]
        S1["liburing / io_uring"]
        S2["Linux socket API"]
        S3["OpenSSL"]
    end

    L1 --> SYS
    L2 --> L1
    L3 --> L2
    C3_tcp -.->|"view()"| C2_stream
    C3_buf -.->|"view()"| C2_buf
```

## The Ownership Dimension

Each layer is further characterized by whether its types **own** resources or
are **non-owning references**:

| Layer | Non-owning Views | RAII Owners |
|-------|-----------------|-------------|
| Layer 1 (`base`) | `submission_queue_entry`, `completion_queue_entry` | `ring`, `probe` |
| Layer 2 (`async_io`) | `buffer_view`, `descriptor_view`, `stream_socket_view`, `listening_socket_view` | *(none — by design)* |
| Layer 3 (`io_context`) | `mutable_buffer`, `const_buffer`, `dynamic_string_buffer` | `tcp_socket`, `tcp_acceptor`, `ssl_context`, `ssl_stream`, `io_context` |

**Key invariant:** Layer 2 (`bupp::async_io`) deliberately contains **no RAII
owners**. Every type is either a non-owning view or a pure value type. This
keeps the layer reusable by any higher-level framework without hidden ownership
coupling.

## Layer Dependency Graph

```mermaid
graph LR
    L3["io_context"] -->|"uses"| L2["async_io"]
    L2 -->|"uses"| L1["base"]
    L1 -->|"uses"| LIB["liburing"]
    L3 -->|"uses"| SSL["OpenSSL"]
    L2 -->|"uses"| LIB2["liburing<br/>(linux_native operations)"]
```

## Platform Native Backends

The current implementation is Linux-only and uses `io_uring`. The planned
macOS/BSD port keeps the same three-layer model but adds a `kqueue` backend
instead of treating `io_uring` as the permanent shape of every platform.

See [`kqueue-roadmap.md`](../kqueue-roadmap.md) for the macOS/BSD technical route.

---
