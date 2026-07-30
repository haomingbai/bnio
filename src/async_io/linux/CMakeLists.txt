target_sources(
  bnio
  PRIVATE address.cpp
          dns.cpp
          io_uring_context.cpp
          io_uring_context_cqe.cpp
          io_uring_context_io_tasks.cpp
          io_uring_context_run_loop.cpp
          io_uring_context_task_queue.cpp
          socket_address.cpp
          socket_view.cpp)
