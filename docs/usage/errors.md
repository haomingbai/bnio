# Error Handling

```cpp
// Base layer: ≥ 0 on success, negative errno on failure
int r = ring.submit();
if (r < 0) {
    // r is -errno (e.g. -EINVAL, -ENOMEM)
}

// io_context layer: std::error_code via set_error completion
// Senders deliver errors through set_error(receiver, std::error_code)
```

Base-layer tests treat `-ENOSYS`, `-EPERM`, and `-EACCES` as covered error
paths for environments where io_uring is unavailable.

## Common Pitfalls

1. **Using CQE fields after `cqe_seen()`** — always read `res()`, `get_data64()`, etc. before calling `cqe_seen()`.

2. **Forgetting `cqe_seen()`** — the CQ ring fills up and no new completions arrive.

3. **Not calling `ctx.run()`** — operations are submitted but the event loop never runs, so completions are never delivered.

4. **Stack buffer passed to async operation** — the buffer must outlive the operation. Use `shared_ptr` or ensure the buffer is in an outer scope.

5. **`ssl_context` destroyed before `ssl_stream`** — the stream's `SSL*` is derived from the context's `SSL_CTX*`.

6. **Copying move-only types** — `ring`, `tcp_socket`, `tcp_acceptor`, `ssl_context`, `ssl_stream`, `io_context` are all move-only.
