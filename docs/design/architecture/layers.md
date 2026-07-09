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
        C1_linux["Linux: ring, submission_queue_entry<br/>completion_queue_entry, probe, params"]
        C1_bsd["BSD: kqueue, event, event_list_view"]
    end

    subgraph SYS["System"]
        S1["Linux: liburing / io_uring"]
        S2["BSD: kqueue / kevent"]
        S3["Linux socket API"]
        S4["OpenSSL"]
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
| Layer 1 (`base`) | `submission_queue_entry`, `completion_queue_entry`, `event`, `event_list_view` | `ring`, `probe`, `kqueue` |
| Layer 2 (`async_io`) | `buffer_view`, `descriptor_view`, `stream_socket_view`, `listening_socket_view` | `linux_native::io_uring_context` |
| Layer 3 (`io_context`) | `mutable_buffer`, `const_buffer`, `dynamic_string_buffer` | `tcp_socket`, `tcp_acceptor`, `ssl_context`, `ssl_stream`, `io_context` |

**Key invariant:** Layer 2 (`bupp::async_io`) vocabulary types are deliberately
non-owning views or pure value types. The `linux_native::io_uring_context` is
the exception — it is the platform-level RAII event-loop owner, one per run-loop
thread. It lives inside `io_context`'s native worker slots.

## Layer Dependency Graph

```mermaid
graph LR
    L3["io_context"] -->|"uses"| L2["async_io"]
    L2 -->|"uses"| L1["base"]
    L1 -->|"uses"| LIB["liburing"]
    L3 -->|"uses"| SSL["OpenSSL"]
    L2 -->|"uses"| LIB2["Linux: liburing<br/>(linux_native operations)"]
    L2 -.->|"planned"| BSD["BSD: kqueue<br/>(bsd_native operations)"]
```

## Platform Native Backends

The primary implementation is Linux with `io_uring`. The library uses a
**one-thread-one-uring** model: each thread calling `io_context::run()` owns its
own `io_uring_context` instance, allocated from a pool sized by
`concurrency_hint`. High-level `post` work is distributed round-robin across
native slots; per-connection I/O stays ring-local after handoff.

BSD kqueue **base wrappers** (`base::kqueue`, `base::event`,
`base::event_list_view`) are already implemented. The full `kqueue_context`
backend and high-level `io_context` integration remain in progress.

See [`kqueue-roadmap.md`](../kqueue-roadmap.md) for the macOS/BSD technical route.

---
