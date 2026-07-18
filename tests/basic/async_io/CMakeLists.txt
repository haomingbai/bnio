function(bnio_add_basic_async_io_test name source)
  add_executable(bnio_basic_async_io_${name}_test ${source})
  target_link_libraries(bnio_basic_async_io_${name}_test PRIVATE bnio::bnio)
  bnio_add_gtest(bnio_basic_async_io_${name}_test bnio.basic.async_io.${name}
                 PROPERTIES LABELS "basic;async_io;${name}" ${ARGN})
endfunction()

bnio_add_basic_async_io_test(address address_test.cpp)
bnio_add_basic_async_io_test(buffer_view buffer_view_test.cpp)
bnio_add_basic_async_io_test(descriptor_view descriptor_view_test.cpp)
bnio_add_basic_async_io_test(dns_query dns_query_test.cpp)
bnio_add_basic_async_io_test(socket_view socket_view_test.cpp)
bnio_add_basic_async_io_test(tcp_endpoint tcp_endpoint_test.cpp)
bnio_add_basic_async_io_test(time time_test.cpp)
if(BNIO_PLATFORM_LINUX)
  bnio_add_basic_async_io_test(io_uring_operation_traits
                               io_uring_operation_traits_test.cpp)
  bnio_add_basic_async_io_test(linux_socket_address
                               linux_socket_address_test.cpp)
endif()
if(BNIO_PLATFORM_BSD)
  bnio_add_basic_async_io_test(kqueue_helper kqueue_helper_test.cpp)
  bnio_add_basic_async_io_test(kqueue_operation_traits
                               kqueue_operation_traits_test.cpp)
  bnio_add_basic_async_io_test(bsd_socket_address bsd_socket_address_test.cpp)
endif()

function(bnio_add_basic_async_io_header_test name definition)
  add_executable(bnio_basic_async_io_header_${name}_test
                 header_self_contained_test.cpp)
  target_compile_definitions(bnio_basic_async_io_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bnio_basic_async_io_header_${name}_test PRIVATE
                        bnio::bnio)
  bnio_add_gtest(bnio_basic_async_io_header_${name}_test
                 bnio.basic.async_io.header.${name})
endfunction()

bnio_add_basic_async_io_header_test(async_io BNIO_HEADER_TEST_ASYNC_IO)
bnio_add_basic_async_io_header_test(address BNIO_HEADER_TEST_ADDRESS)
bnio_add_basic_async_io_header_test(buffer_view BNIO_HEADER_TEST_BUFFER_VIEW)
bnio_add_basic_async_io_header_test(descriptor_view
                                    BNIO_HEADER_TEST_DESCRIPTOR_VIEW)
bnio_add_basic_async_io_header_test(dns BNIO_HEADER_TEST_DNS)
bnio_add_basic_async_io_header_test(ip_address BNIO_HEADER_TEST_IP_ADDRESS)
bnio_add_basic_async_io_header_test(ip_endpoint BNIO_HEADER_TEST_IP_ENDPOINT)
bnio_add_basic_async_io_header_test(ip_tcp BNIO_HEADER_TEST_IP_TCP)
bnio_add_basic_async_io_header_test(ip_udp BNIO_HEADER_TEST_IP_UDP)
if(BNIO_PLATFORM_LINUX)
  bnio_add_basic_async_io_header_test(linux_io_uring_context
                                      BNIO_HEADER_TEST_LINUX_IO_URING_CONTEXT)
  bnio_add_basic_async_io_header_test(linux_socket_address
                                      BNIO_HEADER_TEST_LINUX_SOCKET_ADDRESS)
endif()
if(BNIO_PLATFORM_BSD)
  bnio_add_basic_async_io_header_test(bsd_kqueue_context
                                      BNIO_HEADER_TEST_BSD_KQUEUE_CONTEXT)
  bnio_add_basic_async_io_header_test(bsd_kqueue_helper
                                      BNIO_HEADER_TEST_BSD_KQUEUE_HELPER)
  bnio_add_basic_async_io_header_test(bsd_socket_address
                                      BNIO_HEADER_TEST_BSD_SOCKET_ADDRESS)
endif()
bnio_add_basic_async_io_header_test(socket_view BNIO_HEADER_TEST_SOCKET_VIEW)
bnio_add_basic_async_io_header_test(tcp_endpoint BNIO_HEADER_TEST_TCP_ENDPOINT)
bnio_add_basic_async_io_header_test(time BNIO_HEADER_TEST_TIME)
