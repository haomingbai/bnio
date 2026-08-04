find_package(Threads REQUIRED)

function(bnio_add_integration_io_context_test name source)
  add_executable(bnio_integration_io_context_${name}_test ${source})
  target_link_libraries(bnio_integration_io_context_${name}_test
                        PRIVATE bnio::bnio Threads::Threads)
  bnio_add_gtest(bnio_integration_io_context_${name}_test
                 bnio.integration.io_context.${name}
                 PROPERTIES LABELS "integration;io_context;${name}" ${ARGN})
endfunction()

bnio_add_integration_io_context_test(dns dns_test.cpp)
bnio_add_integration_io_context_test(io_context_accept_connect
                                     io_context_accept_connect_test.cpp)
bnio_add_integration_io_context_test(io_context_poll
                                     io_context_poll_test.cpp)
bnio_add_integration_io_context_test(io_context_read_write
                                     io_context_read_write_test.cpp)
bnio_add_integration_io_context_test(io_context_scheduler
                                     io_context_scheduler_test.cpp
                                     PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(ssl_handshake ssl_handshake_test.cpp)
bnio_add_integration_io_context_test(ssl_transfer ssl_transfer_test.cpp)
bnio_add_integration_io_context_test(steady_timer steady_timer_test.cpp
                                     PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(tcp tcp_test.cpp)
bnio_add_integration_io_context_test(udp udp_test.cpp)
bnio_add_integration_io_context_test(
  lifecycle_stop_run_uaf
  lifecycle/stop_run_uaf_test.cpp
  PROPERTIES TIMEOUT 120)
bnio_add_integration_io_context_test(
  lifecycle_finish_phase3_operation_leak
  lifecycle/finish_phase3_operation_leak_test.cpp)
bnio_add_integration_io_context_test(
  lifecycle_stop_operation_completion
  lifecycle/stop_operation_completion_test.cpp
  PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(
  lifecycle_submit_after_stop
  lifecycle/submit_after_stop_test.cpp
  PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(
  lifecycle_join
  lifecycle/join_test.cpp
  PROPERTIES TIMEOUT 10)
