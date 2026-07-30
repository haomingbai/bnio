target_sources(
  bnio
  PRIVATE address.cpp
          dns.cpp
          kqueue_context.cpp
          kqueue_context_events.cpp
          kqueue_context_io_tasks.cpp
          kqueue_context_run_loop.cpp
          kqueue_context_task_queue.cpp
          kqueue_helper.cpp
          socket_address.cpp
          socket_view.cpp)
