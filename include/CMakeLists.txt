set(BUPP_PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/address.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/buffer_view.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/descriptor_view.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/dns.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/ip/address.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/ip/endpoint.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/ip/tcp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/ip/udp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/socket_view.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/tcp_endpoint.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/time.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/buffer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/bupp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/config/system.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/export.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bupp/ip.h
    PARENT_SCOPE)

# Linux-only public headers. These pull in <liburing.h> and the io_uring
# backend, and must not be exposed on non-Linux platforms.
set(BUPP_LINUX_PUBLIC_HEADERS "" PARENT_SCOPE)
if(BUPP_PLATFORM_LINUX)
  set(BUPP_LINUX_PUBLIC_HEADERS
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/linux/detail/io_uring_receiver_operation.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/linux/io_uring_context.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/linux/io_uring_context_base.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/linux/io_uring_operations.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/async_io/linux/socket_address.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/linux/completion_queue_entry.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/linux/liburing.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/linux/params.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/linux/probe.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/linux/ring.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/linux/submission_queue_entry.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/detail/io_context/scheduler_operations.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/detail/ssl/async_operations.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/detail/ssl/async_operations/common.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/detail/ssl/async_operations/handshake.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/detail/ssl/async_operations/read_write.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/detail/ssl/async_operations/senders.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/detail/ssl/async_operations/shutdown.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/detail/ssl/async_operations/state_machine.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/detail/tcp/async_operations.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/io_context/config.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/io_context.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/io_context_cpo.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/linux/detail/io_context_native_io.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/linux/io_context.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/ssl.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/tcp.h
      PARENT_SCOPE)
endif()

# BSD-only public headers. These pull in <sys/event.h> and are exposed only
# when the kqueue base backend is enabled.
set(BUPP_BSD_PUBLIC_HEADERS "" PARENT_SCOPE)
if(BUPP_PLATFORM_BSD)
  set(BUPP_BSD_PUBLIC_HEADERS
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/bsd/event.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/bsd/event_list_view.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bupp/base/bsd/kqueue.h
      PARENT_SCOPE)
endif()
