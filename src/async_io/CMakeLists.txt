target_sources(
  bupp
  PRIVATE address.cpp
          tcp_endpoint.cpp)

if(BUPP_PLATFORM_LINUX)
  target_sources(
    bupp
    PRIVATE linux/address.cpp
            linux/dns.cpp
            linux/io_uring_context.cpp
            linux/io_uring_context_cqe.cpp
            linux/io_uring_context_io_tasks.cpp
            linux/io_uring_context_run_loop.cpp
            linux/io_uring_context_task_queue.cpp
            linux/socket_address.cpp
            socket_view.cpp)
endif()

if(BUPP_PLATFORM_BSD)
  target_sources(
    bupp
    PRIVATE bsd/kqueue_context.cpp
            bsd/kqueue_context_events.cpp
            bsd/kqueue_context_run_loop.cpp
            bsd/kqueue_context_submission.cpp
            bsd/kqueue_context_task_queue.cpp
            bsd/kqueue_helper.cpp)
endif()
