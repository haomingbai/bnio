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
bnio_add_integration_io_context_test(local_stream_peer_close
                                     local_stream_peer_close_test.cpp)
bnio_add_integration_io_context_test(local_stream_lifecycle
                                     local_stream_lifecycle_test.cpp)
bnio_add_integration_io_context_test(local_io_context
                                     local_io_context_test.cpp)
bnio_add_integration_io_context_test(local_datagram local_datagram_test.cpp)
bnio_add_integration_io_context_test(local_error local_error_test.cpp)
bnio_add_integration_io_context_test(io_context_scheduler
                                     io_context_scheduler_test.cpp
                                     PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(io_context_defer_scheduler
                                     io_context_defer_scheduler_test.cpp
                                     PROPERTIES TIMEOUT 20)
bnio_add_integration_io_context_test(ssl_error_precedence
                                     ssl_error_precedence_test.cpp
                                     PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(ssl_error_queue_hygiene
                                     ssl_error_queue_hygiene_test.cpp
                                     PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(ssl_handshake ssl_handshake_test.cpp)
bnio_add_integration_io_context_test(ssl_handshake_retry
                                     ssl_handshake_retry_test.cpp
                                     PROPERTIES TIMEOUT 30)
bnio_add_integration_io_context_test(ssl_handshake_retry_category
                                     ssl_handshake_retry_category_test.cpp
                                     PROPERTIES TIMEOUT 10)
target_sources(
  bnio_integration_io_context_ssl_handshake_retry_category_test
  PRIVATE ssl_handshake_retry_category_peer.cpp)
bnio_add_integration_io_context_test(ssl_transfer ssl_transfer_test.cpp)
bnio_add_integration_io_context_test(ssl_alpn ssl_alpn_test.cpp)
# The ALPN context-lifetime regressions (context destroyed or moved while
# streams are still live) are only meaningful when a use-after-free is
# actually detected, so this target is instrumented with AddressSanitizer.
# The context and closure code lives in headers, which instruments the code
# under test through this translation unit.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT BNIO_ENABLE_TSAN)
  target_compile_options(
    bnio_integration_io_context_ssl_alpn_test
    PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(
    bnio_integration_io_context_ssl_alpn_test
    PRIVATE -fsanitize=address)
endif()
bnio_add_integration_io_context_test(ssl_sni ssl_sni_test.cpp
                                     PROPERTIES TIMEOUT 30)
bnio_add_integration_io_context_test(ssl_client_hello
                                     ssl_client_hello_test.cpp
                                     PROPERTIES TIMEOUT 30)
bnio_add_integration_io_context_test(ssl_cert_callback
                                     ssl_cert_callback_test.cpp
                                     PROPERTIES TIMEOUT 30)
# The callback-lifetime tests move and destroy context owners while streams
# are still live, so they are only meaningful under AddressSanitizer (same
# pattern as ALPN). The closure code lives in headers and is instrumented
# through these translation units.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT BNIO_ENABLE_TSAN)
  foreach(target
          bnio_integration_io_context_ssl_sni_test
          bnio_integration_io_context_ssl_client_hello_test
          bnio_integration_io_context_ssl_cert_callback_test)
    target_compile_options(${target}
                           PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endforeach()
endif()
bnio_add_integration_io_context_test(ssl_context_switch
                                     ssl_context_switch_test.cpp)
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
  lifecycle_stop_nonblocking
  lifecycle/stop_nonblocking_test.cpp
  PROPERTIES TIMEOUT 15)
bnio_add_integration_io_context_test(
  lifecycle_stop_wakes_all_workers
  lifecycle/stop_wakes_all_workers_test.cpp
  PROPERTIES TIMEOUT 60)
bnio_add_integration_io_context_test(
  lifecycle_nested_run
  lifecycle/nested_run_test.cpp
  PROPERTIES TIMEOUT 10)
bnio_add_integration_io_context_test(
  lifecycle_request_stop
  lifecycle/request_stop_test.cpp
  PROPERTIES TIMEOUT 10)
# Fork + watchdog: a lost transition parks the child instead of the runner.
bnio_add_integration_io_context_test(
  lifecycle_join_stop_race
  lifecycle/join_stop_race_test.cpp
  PROPERTIES TIMEOUT 20)
bnio_add_integration_io_context_test(
  lifecycle_stop_transition_race
  lifecycle/stop_transition_race_test.cpp
  PROPERTIES TIMEOUT 20)
# Fork + watchdog: publishers race the Phase-2 abort walk, which would park
# the child rather than the runner if a transition were lost.
bnio_add_integration_io_context_test(
  lifecycle_aborting_publish_race
  lifecycle/aborting_publish_race_test.cpp
  PROPERTIES TIMEOUT 30)
# The race body runs in a forked child and exits with _exit(), which skips
# the gcov atexit flush. Let it dump the child's counters explicitly when the
# target is instrumented.
if(BNIO_ENABLE_COVERAGE)
  target_compile_definitions(
    bnio_integration_io_context_lifecycle_aborting_publish_race_test
    PRIVATE BNIO_TEST_COVERAGE=1)
endif()
# Closes/flips fd 0 on purpose, so it must not run alongside other tests.
bnio_add_integration_io_context_test(
  lifecycle_accept_abort_fd_zero
  lifecycle/accept_abort_fd_zero_test.cpp
  PROPERTIES RUN_SERIAL TRUE TIMEOUT 20)
if(BNIO_PLATFORM_LINUX)
  # Signal-driven request_stop() regression test: uses SIGUSR1 and a forked
  # child, so it must not run concurrently with other tests. Linux-only
  # because the defect lived in the io_uring wait path.
  bnio_add_integration_io_context_test(
    lifecycle_stop_from_signal_handler
    lifecycle/stop_from_signal_handler_test.cpp
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 20)
endif()
