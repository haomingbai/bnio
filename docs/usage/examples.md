# Examples

For runnable commands and the optional benchmark setup, see
[`examples.md`](../examples.md).

### Base Layer — NOP Request

Minimal ring → SQE → submit → CQE cycle:

```cpp
#include <bupp/base.h>

int main() {
    bupp::base::ring ring;
    if (ring.queue_init(8) < 0) return 1;

    bupp::base::submission_queue_entry sqe = ring.get_sqe();
    if (sqe.raw() == nullptr) return 1;

    sqe.prep_nop();
    sqe.set_data64(42);

    if (ring.submit() < 0) return 1;

    bupp::base::completion_queue_entry cqe;
    if (ring.wait_cqe(cqe) < 0) return 1;

    int result = cqe.res();
    ring.cqe_seen(cqe);
    return result;
}
```

### io_context Layer — Async Read

```cpp
#include <bupp/bupp.h>
#include <iostream>
#include <array>

struct print_receiver {
    void set_value(std::size_t n) {
        std::cout << "transferred " << n << " bytes\n";
    }
    void set_error(std::error_code ec) {
        std::cerr << "error: " << ec.message() << '\n';
    }
    void set_stopped() {
        std::cout << "stopped\n";
    }
};

int main() {
    bupp::io_context ctx;
    auto scheduler = ctx.get_post_scheduler();

    bupp::tcp_socket sock;
    sock.open(bupp::ip::tcp::v4());

    auto ep = bupp::ip::endpoint(
        bupp::ip::make_v4_address("127.0.0.1"), 7000);
    sock.view().connect(ep);

    std::array<char, 4096> buf{};
    auto sender = sock.async_read(scheduler, bupp::buffer(buf), 0);
    auto op = std::move(sender).connect(print_receiver{});
    op.start();

    ctx.run();
    return 0;
}
```

### Base Layer — Echo Server

Full echo server at the base layer, demonstrating the raw ring/SQE/CQE
dispatch pattern with manual accept → recv → echo send loops:

[`examples/base/linux/echo_server.cpp`](../../examples/base/linux/echo_server.cpp)

### io_context Layer — Raw Echo Server

Raw TCP echo server at the `io_context` layer, demonstrating repeated
`async_accept`, per-connection `async_read`/`async_write`, detached operation
lifetime through a local holder, and `ctx.run()` as the server event loop:

[`examples/raw_echo`](../../examples/raw_echo)

### Choosing the Right Layer

| Use Case | Recommended Layer |
|----------|------------------|
| Full control over io_uring | `base` |
| Need platform-neutral vocabulary types | `async_io` |
| Building application network services | `io_context` |
| Integrating with sender/receiver frameworks | `io_context` + CPOs |
