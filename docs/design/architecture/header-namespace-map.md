# Header Dependency Graph

```
bnio/bnio.h  (umbrella)
├── bnio/base.h
│   ├── base/linux/{ring, submission_queue_entry, completion_queue_entry,
│   │               params, probe, liburing}.h
│   └── base/bsd/{kqueue, event, event_list_view}.h
├── bnio/async_io.h
│   ├── async_io/{buffer_view, descriptor_view, random_access_file,
│   │             socket_view, time, address, tcp_endpoint, config}.h
│   ├── async_io/ip/{address, endpoint, tcp, udp}.h
│   └── async_io/dns/{query, result, resolve, types}.h
├── bnio/io_context.h
│   ├── detail/posix/io_context/class.h
│   ├── detail/posix/io_context/native_context.h
│   │   ├── async_io/linux/io_uring_context.h
│   │   └── async_io/bsd/kqueue_context.h
│   ├── detail/posix/io_context/{options,
│   │                      timer_types, steady_timer}.h
│   └── detail/posix/io_context/native_io.h
│       ├── detail/posix/io_context/{timer_wait, descriptor_file_io,
│       │                            random_access_file_io}.h
│       ├── detail/posix/io_context/read_all.h
│       │   └── detail/posix/io_context/{descriptor_read_all,
│       │                                random_access_read_all}.h
│       ├── detail/posix/io_context/write_all.h
│       │   └── detail/posix/io_context/{descriptor_write_all,
│       │                                random_access_write_all}.h
│       ├── detail/linux/io_context_native_io/{common, factories,
│       │                     file_factories, file, descriptor_file,
│       │                     random_access_file, poll, socket}.h
│       └── detail/bsd/io_context_native_io/{common, factories,
│                                            file_factories}.h
├── bnio/async_io/linux/io_uring_context.h
│   ├── async_io/linux/io_uring_context_base.h
│   │   ├── async_io/linux/io_uring_context_base/context.h
│   │   ├── async_io/linux/io_uring_context_base/operation_base.h
│   │   └── async_io/linux/io_uring_context_base/options.h
│   └── async_io/linux/io_uring_operations.h
│       ├── async_io/linux/io_uring_operations/{core, helpers, poll,
│       │     resolve, socket, views}.h
│       └── async_io/linux/io_uring_operations/file.h
│           └── async_io/linux/io_uring_operations/{descriptor_file,
│               random_access_file}.h
├── bnio/async_io/bsd/kqueue_context.h
│   ├── async_io/bsd/kqueue_context_base.h
│   ├── async_io/bsd/kqueue_helper.h
│   ├── async_io/bsd/kqueue_operations/{core, poll, resolve,
│   │   socket}.h
│   └── async_io/bsd/kqueue_operations/file.h
│       └── async_io/bsd/kqueue_operations/{file_common,
│           descriptor_file, random_access_file}.h
├── bnio/ip.h
├── bnio/buffer.h
│   └── buffer/{basic, dynamic_string, dynamic_byte_vector, holders}.h
├── bnio/tcp.h
│   └── tcp/{socket, acceptor, async_operations, layers}.h
├── bnio/ssl.h
│   └── ssl/{context, stream_class, stream, stream_operations, cpo}.h
├── bnio/io_context_cpo.h
│   └── io_context_cpo/{instances, concepts, read, write,
│                       connection, poll, resolve}.h
└── bnio/export.h
```

For smaller translation units, include individual sub-headers (e.g.
`bnio/base.h`) instead of `bnio/bnio.h`.

## Namespace Map

```
bnio
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
│   ├── descriptor_view                   non-owning fd (streaming I/O)
│   ├── random_access_file                non-owning fd (positioned I/O)
│   ├── socket_view                       non-owning generic socket
│   ├── stream_socket_view                non-owning SOCK_STREAM socket
│   ├── datagram_socket_view              non-owning SOCK_DGRAM socket
│   ├── ip
│   │   ├── address                       IPv4/IPv6 address
│   │   ├── endpoint                      address + port
│   │   ├── tcp                           TCP protocol tag
│   │   └── udp                           UDP protocol tag
│   ├── dns_query                         DNS query description
│   ├── dns_result_view                   DNS result storage view
│   ├── linux_native                      Linux-specific event loop
│   │   ├── socket_address                Linux-native sockaddr storage
│   │   ├── io_uring_context              platform event-loop owner (non-movable)
│   │   ├── io_uring_context_options      context configuration
│   │   ├── io_uring_operation_base       intrusive operation node
│   │   ├── io_uring_task_queue_state     shared queues + wake/stopping state
│   │   └── io_uring_*_operation          concrete I/O operations
│   └── bsd_native                        BSD-specific event loop
│       ├── socket_address                BSD-native sockaddr storage
│       ├── kqueue_context                platform event-loop owner
│       ├── kqueue_context_options        context configuration
│       ├── kqueue_operation_base         intrusive CPU operation node
│       ├── kqueue_io_operation_base      readiness operation node
│       └── kqueue_task_queue_state       shared queues + wake/stopping state
├── io_context                            Layer 3: high-level async context
│   ├── operation_base                    intrusive shared-I/O node
│   ├── basic_scheduler<Kind>             dispatch/post scheduler handles
│   └── schedule_sender<Kind>             scheduler sender types
├── detail                                Layer 3 internals
│   ├── timer_state_data                  timer heap/list + passive timeout state
│   └── *_write_all_state                 layer-2 sender compositions
├── ip
│   ├── address (= async_io::ip::address)
│   ├── endpoint (= async_io::ip::endpoint)
│   ├── tcp                               protocol tag + socket type aliases
│   └── udp                               protocol tag + UDP socket alias
├── tcp
│   ├── socket                            RAII TCP stream socket owner
│   └── acceptor                          RAII TCP listening socket owner
├── udp
│   └── socket                            RAII UDP datagram socket owner
├── tcp_socket / tcp_acceptor             compatibility aliases
├── udp_socket                            compatibility alias
├── ssl_context                           RAII SSL_CTX owner
├── ssl_stream<NextLayer>                 RAII SSL + BIO + transport owner
├── mutable_buffer                        non-owning mutable byte view
├── const_buffer                          non-owning const byte view
├── dynamic_string_buffer                 dynamic buffer adapter (std::string)
├── dynamic_byte_vector_buffer<A>         dynamic buffer adapter (std::vector<std::byte>)
└── steady_timer                          io_context-bound timer object
```
