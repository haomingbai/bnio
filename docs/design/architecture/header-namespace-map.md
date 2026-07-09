# Header Dependency Graph

```
bupp/bupp.h  (umbrella)
├── bupp/base.h
│   └── base/linux/{ring, submission_queue_entry, completion_queue_entry,
│                   params, probe}.h
├── bupp/async_io.h
│   ├── async_io/{buffer_view, descriptor_view, socket_view, time}.h
│   └── async_io/ip/{address, endpoint, tcp, udp}.h
├── bupp/io_context.h
│   └── linux/io_context.h
│       └── async_io/linux/io_uring_context.h
│           ├── async_io/linux/io_uring_context_base.h
│           └── async_io/linux/io_uring_operations.h
│               └── async_io/linux/io_uring_operations/{core, file,
│                   helpers, poll, resolve, socket, views}.h
├── bupp/ip.h
├── bupp/buffer.h
├── bupp/tcp.h
├── bupp/ssl.h
├── bupp/io_context_cpo.h
└── bupp/export.h
```

For smaller translation units, include individual sub-headers (e.g.
`bupp/base.h`) instead of `bupp/bupp.h`.

## Namespace Map

```
bupp
├── base                                  Layer 1: liburing wrappers
│   ├── ring                              io_uring RAII owner
│   ├── submission_queue_entry            io_uring_sqe* non-owning view
│   ├── completion_queue_entry            io_uring_cqe* non-owning view
│   ├── params                            io_uring_params value type
│   └── probe                             io_uring_probe RAII owner
├── async_io                              Layer 2: vocabulary types
│   ├── buffer_view                       non-owning byte range
│   ├── descriptor_view                   non-owning fd
│   ├── socket_view                       non-owning generic socket
│   ├── listening_socket_view             non-owning passive socket
│   ├── stream_socket_view                non-owning active socket
│   ├── ip
│   │   ├── address                       IPv4/IPv6 address
│   │   ├── endpoint                      address + port
│   │   ├── tcp                           TCP protocol tag
│   │   └── udp                           UDP protocol tag
│   └── linux_native                      Linux-specific operations
│       ├── io_uring_context              platform event-loop owner
│       ├── io_uring_operation_base       intrusive operation node
│       └── io_uring_*_operation          concrete I/O operations
├── io_context                            Layer 3: high-level async context
│   └── operation_base                    intrusive queued-I/O node
├── ip
│   ├── address (= async_io::ip::address)
│   ├── endpoint (= async_io::ip::endpoint)
│   ├── tcp                               protocol tag + socket type aliases
│   └── udp (= async_io::ip::udp)
├── tcp_socket                            RAII TCP stream socket owner
├── tcp_acceptor                          RAII TCP listening socket owner
├── ssl_context                           RAII SSL_CTX owner
├── ssl_stream<NextLayer>                 RAII SSL + BIO + transport owner
├── mutable_buffer                        non-owning mutable byte view
├── const_buffer                          non-owning const byte view
├── dynamic_string_buffer                 dynamic buffer adapter (std::string)
└── dynamic_byte_vector_buffer<A>         dynamic buffer adapter (std::vector<std::byte>)
```
