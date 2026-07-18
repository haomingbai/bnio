target_sources(
  bnio
  PRIVATE address.cpp
          tcp_endpoint.cpp)

if(BNIO_PLATFORM_LINUX)
  target_sources(
    bnio
    PRIVATE linux/address.cpp
            linux/dns.cpp
            linux/io_uring_context.cpp
            linux/io_uring_context_cqe.cpp
            linux/io_uring_context_io_tasks.cpp
            linux/io_uring_context_run_loop.cpp
            linux/io_uring_context_task_queue.cpp
            linux/socket_address.cpp
            linux/socket_view.cpp)
endif()

if(BNIO_PLATFORM_BSD)
  target_sources(
    bnio
    PRIVATE bsd/address.cpp
            bsd/dns.cpp
            bsd/kqueue_context.cpp
            bsd/kqueue_context_events.cpp
            bsd/kqueue_context_io_tasks.cpp
            bsd/kqueue_context_run_loop.cpp
            bsd/kqueue_context_task_queue.cpp
            bsd/kqueue_helper.cpp
            bsd/socket_address.cpp
            bsd/socket_view.cpp)
endif()
