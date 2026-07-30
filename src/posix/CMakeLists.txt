target_sources(
  bnio
  PRIVATE io_context.cpp
          io_context_queue.cpp
          io_context_timer.cpp
          io_context_timer_state.cpp
          ssl.cpp
          tcp.cpp
          udp.cpp)
