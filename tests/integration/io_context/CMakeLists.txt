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
bnio_add_integration_io_context_test(io_context_random_access_file
                                     io_context_random_access_file_test.cpp)
bnio_add_integration_io_context_test(io_context_eager_optional
                                     io_context_eager_optional_test.cpp)
bnio_add_integration_io_context_test(io_context_read_all
                                     io_context_read_all_test.cpp)
bnio_add_integration_io_context_test(io_context_read_write
                                     io_context_read_write_test.cpp)
bnio_add_integration_io_context_test(io_context_scheduler
                                     io_context_scheduler_test.cpp
                                     PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(ssl_error_precedence
                                     ssl_error_precedence_test.cpp
                                     PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(ssl_error_queue_hygiene
                                     ssl_error_queue_hygiene_test.cpp
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
  lifecycle_finish_no_new_io_after_stop
  lifecycle/finish_no_new_io_after_stop_test.cpp
  PROPERTIES TIMEOUT 20)
bnio_add_integration_io_context_test(
  lifecycle_stop_operation_completion
  lifecycle/stop_operation_completion_test.cpp
  PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(
  lifecycle_submit_after_stop
  lifecycle/submit_after_stop_test.cpp
  PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(
  lifecycle_stop_without_worker_drains_queues
  lifecycle/stop_without_worker_drains_queues_test.cpp
  PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(
  lifecycle_join
  lifecycle/join_test.cpp
  PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(
  lifecycle_nested_run
  lifecycle/nested_run_test.cpp
  PROPERTIES TIMEOUT 10)
# Closes/flips fd 0 on purpose, so it must not run alongside other tests.
bnio_add_integration_io_context_test(
  lifecycle_accept_abort_fd_zero
  lifecycle/accept_abort_fd_zero_test.cpp
  PROPERTIES RUN_SERIAL TRUE TIMEOUT 20)
if(BNIO_PLATFORM_LINUX)
  # Signal-driven stop() regression test: uses SIGUSR1 and a forked child,
  # so it must not run concurrently with other tests. Linux-only because the
  # defect lived in the io_uring wait path.
  bnio_add_integration_io_context_test(
    lifecycle_stop_from_signal_handler
    lifecycle/stop_from_signal_handler_test.cpp
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 20)
endif()
