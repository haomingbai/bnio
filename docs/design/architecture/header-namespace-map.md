# Header Dependency Graph

```
bupp/bupp.h  (umbrella)
├── bupp/base.h
│   ├── base/linux/{ring, submission_queue_entry, completion_queue_entry,
│   │               params, probe, liburing}.h
│   └── base/bsd/{kqueue, event, event_list_view}.h
├── bupp/async_io.h
│   ├── async_io/{buffer_view, descriptor_view, socket_view, time,
│   │             address, tcp_endpoint, config}.h
│   ├── async_io/ip/{address, endpoint, tcp, udp}.h
│   └── async_io/dns/{query, result, resolve, types}.h
├── bupp/io_context.h
│   └── linux/io_context.h
│       ├── linux/detail/io_context_options.h
│       ├── linux/detail/io_context_timer_types.h
│       ├── linux/detail/steady_timer.h
│       └── async_io/linux/io_uring_context.h
│           ├── async_io/linux/io_uring_context_base.h
│           │   ├── async_io/linux/io_uring_context_base/context.h
│           │   ├── async_io/linux/io_uring_context_base/operation_base.h
│           │   ├── async_io/linux/io_uring_context_base/options.h
│           │   └── async_io/linux/io_uring_context_base/submission.h
│           └── async_io/linux/io_uring_operations.h
│               └── async_io/linux/io_uring_operations/{core, file,
│                   helpers, poll, resolve, socket, views}.h
├── bupp/ip.h
├── bupp/buffer.h
│   └── buffer/{basic, dynamic_string, dynamic_byte_vector, holders}.h
├── bupp/tcp.h
│   └── tcp/{socket, acceptor, async_operations, layers}.h
├── bupp/ssl.h
│   └── ssl/{context, stream_class, stream, stream_operations, cpo}.h
├── bupp/io_context_cpo.h
│   └── io_context_cpo/{instances, concepts, read, write,
│                       connection, poll, resolve}.h
└── bupp/export.h
```

For smaller translation units, include individual sub-headers (e.g.
`bupp/base.h`) instead of `bupp/bupp.h`.

## Namespace Map

```
bupp
├── base                                  Layer 1: system call wrappers
│   ├── Linux:
│   │   ├── ring                          io_uring RAII owner
│   │   ├── submission_queue_entry        io_uring_sqe* non-owning view
│   │   ├── completion_queue_entry        io_uring_cqe* non-owning view
│   │   ├── params                        io_uring_params value type
│   │   └── probe                         io_uring_probe RAII owner
│   └── BSD:
│       ├── kqueue                        kqueue fd RAII owner
│       ├── event                         struct kevent wrapper
│       └── event_list_view               non-owning kevent array view
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
│   ├── dns_query                         DNS query description
│   ├── dns_result_view                   DNS result storage view
│   └── linux_native                      Linux-specific event loop
│       ├── io_uring_context              platform event-loop owner (non-movable)
│       ├── io_uring_context_options      context configuration
│       ├── io_uring_operation_base       intrusive operation node
│       └── io_uring_*_operation          concrete I/O operations
├── io_context                            Layer 3: high-level async context
│   ├── operation_base                    intrusive queued-I/O node
│   ├── basic_scheduler<Kind>             dispatch/post scheduler handles
│   ├── schedule_sender<Kind>             scheduler sender types
│   └── native_worker                     per-thread io_uring_context slot
├── ip
│   ├── address (= async_io::ip::address)
│   ├── endpoint (= async_io::ip::endpoint)
│   ├── tcp                               protocol tag + socket type aliases
│   └── udp (= async_io::ip::udp)
├── submit_mode                           {queued, direct} submission policy
├── tcp_socket                            RAII TCP stream socket owner
├── tcp_acceptor                          RAII TCP listening socket owner
├── ssl_context                           RAII SSL_CTX owner
├── ssl_stream<NextLayer>                 RAII SSL + BIO + transport owner
├── mutable_buffer                        non-owning mutable byte view
├── const_buffer                          non-owning const byte view
├── dynamic_string_buffer                 dynamic buffer adapter (std::string)
├── dynamic_byte_vector_buffer<A>         dynamic buffer adapter (std::vector<std::byte>)
└── steady_timer                          io_context-bound timer object
```
