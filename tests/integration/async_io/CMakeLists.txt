function(bnio_add_integration_async_io_test name source)
  add_executable(bnio_integration_async_io_${name}_test ${source})
  target_link_libraries(bnio_integration_async_io_${name}_test PRIVATE
                        bnio::bnio)
  bnio_add_gtest(bnio_integration_async_io_${name}_test
                 bnio.integration.async_io.${name}
                 PROPERTIES LABELS "integration;async_io;${name}" ${ARGN})
endfunction()

bnio_add_integration_async_io_test(dns_resolve dns_resolve_test.cpp)
if(BNIO_PLATFORM_LINUX)
  bnio_add_integration_async_io_test(io_uring_cqe_dispatch
                                     io_uring_cqe_dispatch_test.cpp
                                     PROPERTIES TIMEOUT 10)
  bnio_add_integration_async_io_test(io_uring_run_loop
                                     io_uring_run_loop_test.cpp
                                     PROPERTIES TIMEOUT 10)
  bnio_add_integration_async_io_test(io_uring_sender
                                     io_uring_sender_test.cpp
                                     PROPERTIES TIMEOUT 10)
  bnio_add_integration_async_io_test(io_uring_error_routing
                                     io_uring_error_routing_test.cpp
                                     PROPERTIES TIMEOUT 30)
  bnio_add_integration_async_io_test(io_uring_error_branch
                                     io_uring_error_branch_test.cpp
                                     PROPERTIES TIMEOUT 30)
  bnio_add_integration_async_io_test(udp_io_uring udp_io_uring_test.cpp
                                     PROPERTIES TIMEOUT 10)
  bnio_add_integration_async_io_test(io_uring_eagain_rearm
                                     io_uring_eagain_rearm_test.cpp
                                     PROPERTIES TIMEOUT 30)
  bnio_add_integration_async_io_test(io_uring_sq_full_submit_failure
                                     io_uring_sq_full_submit_failure_test.cpp
                                     PROPERTIES TIMEOUT 30)
endif()
if(BNIO_PLATFORM_BSD)
  bnio_add_integration_async_io_test(kqueue_run_loop kqueue_run_loop_test.cpp)
  bnio_add_integration_async_io_test(kqueue_sender kqueue_sender_test.cpp)
endif()
