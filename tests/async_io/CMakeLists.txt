function(bupp_add_async_io_test name source)
  add_executable(bupp_async_io_${name}_test ${source})
  target_link_libraries(bupp_async_io_${name}_test PRIVATE bupp::bupp)
  bupp_set_project_warnings(bupp_async_io_${name}_test)
  add_test(NAME bupp.async_io.${name} COMMAND bupp_async_io_${name}_test)
endfunction()

bupp_add_async_io_test(address address_test.cpp)
bupp_add_async_io_test(buffer_view buffer_view_test.cpp)
bupp_add_async_io_test(descriptor_view descriptor_view_test.cpp)
bupp_add_async_io_test(dns dns_test.cpp)
bupp_add_async_io_test(socket_view socket_view_test.cpp)
if(BUPP_PLATFORM_LINUX)
  bupp_add_async_io_test(io_uring_cqe_dispatch
                         io_uring_cqe_dispatch_test.cpp)
  bupp_add_async_io_test(io_uring_operation_traits
                         io_uring_operation_traits_test.cpp)
  bupp_add_async_io_test(io_uring_run_loop io_uring_run_loop_test.cpp)
  bupp_add_async_io_test(io_uring_sender io_uring_sender_test.cpp)
  bupp_add_async_io_test(linux_socket_address linux_socket_address_test.cpp)
  bupp_add_async_io_test(udp_io_uring udp_io_uring_test.cpp)
endif()
if(BUPP_PLATFORM_BSD)
  bupp_add_async_io_test(bsd_socket_address bsd_socket_address_test.cpp)
  bupp_add_async_io_test(kqueue_helper kqueue_helper_test.cpp)
  bupp_add_async_io_test(kqueue_operation_traits
                         kqueue_operation_traits_test.cpp)
  bupp_add_async_io_test(kqueue_run_loop kqueue_run_loop_test.cpp)
  bupp_add_async_io_test(kqueue_sender kqueue_sender_test.cpp)
endif()
bupp_add_async_io_test(tcp_endpoint tcp_endpoint_test.cpp)
bupp_add_async_io_test(time time_test.cpp)

function(bupp_add_async_io_header_self_contained_test name definition)
  add_executable(bupp_async_io_header_${name}_test
                 header_self_contained_test.cpp)
  target_compile_definitions(bupp_async_io_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bupp_async_io_header_${name}_test PRIVATE bupp::bupp)
  bupp_set_project_warnings(bupp_async_io_header_${name}_test)
  add_test(NAME bupp.async_io.header.${name}
           COMMAND bupp_async_io_header_${name}_test)
endfunction()

bupp_add_async_io_header_self_contained_test(async_io
                                             BUPP_HEADER_TEST_ASYNC_IO)
bupp_add_async_io_header_self_contained_test(address
                                             BUPP_HEADER_TEST_ADDRESS)
bupp_add_async_io_header_self_contained_test(buffer_view
                                             BUPP_HEADER_TEST_BUFFER_VIEW)
bupp_add_async_io_header_self_contained_test(
  descriptor_view BUPP_HEADER_TEST_DESCRIPTOR_VIEW)
bupp_add_async_io_header_self_contained_test(dns BUPP_HEADER_TEST_DNS)
bupp_add_async_io_header_self_contained_test(ip_address
                                             BUPP_HEADER_TEST_IP_ADDRESS)
bupp_add_async_io_header_self_contained_test(ip_endpoint
                                             BUPP_HEADER_TEST_IP_ENDPOINT)
bupp_add_async_io_header_self_contained_test(ip_tcp BUPP_HEADER_TEST_IP_TCP)
bupp_add_async_io_header_self_contained_test(ip_udp BUPP_HEADER_TEST_IP_UDP)
if(BUPP_PLATFORM_LINUX)
  bupp_add_async_io_header_self_contained_test(
    linux_io_uring_context BUPP_HEADER_TEST_LINUX_IO_URING_CONTEXT)
  bupp_add_async_io_header_self_contained_test(
    linux_socket_address BUPP_HEADER_TEST_LINUX_SOCKET_ADDRESS)
endif()
if(BUPP_PLATFORM_BSD)
  bupp_add_async_io_header_self_contained_test(
    bsd_kqueue_context BUPP_HEADER_TEST_BSD_KQUEUE_CONTEXT)
  bupp_add_async_io_header_self_contained_test(
    bsd_kqueue_helper BUPP_HEADER_TEST_BSD_KQUEUE_HELPER)
  bupp_add_async_io_header_self_contained_test(
    bsd_socket_address BUPP_HEADER_TEST_BSD_SOCKET_ADDRESS)
endif()
bupp_add_async_io_header_self_contained_test(socket_view
                                             BUPP_HEADER_TEST_SOCKET_VIEW)
bupp_add_async_io_header_self_contained_test(tcp_endpoint
                                             BUPP_HEADER_TEST_TCP_ENDPOINT)
bupp_add_async_io_header_self_contained_test(time BUPP_HEADER_TEST_TIME)
