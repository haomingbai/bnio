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
            linux/io_uring_context_run_loop.cpp
            linux/io_uring_context_submission.cpp
            linux/io_uring_context_task_queue.cpp
            linux/socket_address.cpp
            socket_view.cpp)
endif()
