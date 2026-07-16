function(bupp_add_integration_async_io_test name source)
  add_executable(bupp_integration_async_io_${name}_test ${source})
  target_link_libraries(bupp_integration_async_io_${name}_test PRIVATE
                        bupp::bupp)
  bupp_add_gtest(bupp_integration_async_io_${name}_test
                 bupp.integration.async_io.${name}
                 PROPERTIES LABELS "integration;async_io;${name}" ${ARGN})
endfunction()

bupp_add_integration_async_io_test(dns_resolve dns_resolve_test.cpp)
if(BUPP_PLATFORM_LINUX)
  bupp_add_integration_async_io_test(io_uring_cqe_dispatch
                                     io_uring_cqe_dispatch_test.cpp
                                     PROPERTIES TIMEOUT 10)
  bupp_add_integration_async_io_test(io_uring_run_loop
                                     io_uring_run_loop_test.cpp
                                     PROPERTIES TIMEOUT 10)
  bupp_add_integration_async_io_test(io_uring_sender
                                     io_uring_sender_test.cpp
                                     PROPERTIES TIMEOUT 10)
  bupp_add_integration_async_io_test(udp_io_uring udp_io_uring_test.cpp
                                     PROPERTIES TIMEOUT 10)
endif()
if(BUPP_PLATFORM_BSD)
  bupp_add_integration_async_io_test(kqueue_run_loop kqueue_run_loop_test.cpp)
  bupp_add_integration_async_io_test(kqueue_sender kqueue_sender_test.cpp)
endif()
