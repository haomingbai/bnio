function(bupp_add_io_context_test name source)
  add_executable(bupp_io_context_${name}_test ${source})
  target_link_libraries(bupp_io_context_${name}_test PRIVATE bupp::bupp)
  bupp_set_project_warnings(bupp_io_context_${name}_test)
  add_test(NAME bupp.io_context.${name}
           COMMAND bupp_io_context_${name}_test)
endfunction()

bupp_add_io_context_test(buffer buffer_test.cpp)
bupp_add_io_context_test(dns dns_test.cpp)
bupp_add_io_context_test(io_context io_context_test.cpp)
bupp_add_io_context_test(ssl ssl_test.cpp)
bupp_add_io_context_test(tcp tcp_test.cpp)

function(bupp_add_io_context_header_test name definition)
  add_executable(bupp_io_context_header_${name}_test
                 header_self_contained_test.cpp)
  target_compile_definitions(bupp_io_context_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bupp_io_context_header_${name}_test PRIVATE bupp::bupp)
  bupp_set_project_warnings(bupp_io_context_header_${name}_test)
  add_test(NAME bupp.io_context.header.${name}
           COMMAND bupp_io_context_header_${name}_test)
endfunction()

bupp_add_io_context_header_test(buffer BUPP_HEADER_TEST_BUFFER)
bupp_add_io_context_header_test(io_context BUPP_HEADER_TEST_IO_CONTEXT)
bupp_add_io_context_header_test(io_context_cpo BUPP_HEADER_TEST_IO_CONTEXT_CPO)
bupp_add_io_context_header_test(ip BUPP_HEADER_TEST_IP)
bupp_add_io_context_header_test(ssl BUPP_HEADER_TEST_SSL)
bupp_add_io_context_header_test(tcp BUPP_HEADER_TEST_TCP)
