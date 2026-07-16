find_package(Threads REQUIRED)

function(bupp_add_basic_io_context_test name source)
  add_executable(bupp_basic_io_context_${name}_test ${source})
  target_link_libraries(bupp_basic_io_context_${name}_test PRIVATE bupp::bupp
                                                                Threads::Threads)
  bupp_add_gtest(bupp_basic_io_context_${name}_test bupp.basic.io_context.${name}
                 PROPERTIES LABELS "basic;io_context;${name}" ${ARGN})
endfunction()

bupp_add_basic_io_context_test(buffer buffer_test.cpp)
bupp_add_basic_io_context_test(io_context_sender_concept
                               io_context_sender_concept_test.cpp)
bupp_add_basic_io_context_test(ssl_concept ssl_concept_test.cpp)

function(bupp_add_basic_io_context_header_test name definition)
  add_executable(bupp_basic_io_context_header_${name}_test
                 header_self_contained_test.cpp)
  target_compile_definitions(bupp_basic_io_context_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bupp_basic_io_context_header_${name}_test PRIVATE
                        bupp::bupp)
  bupp_add_gtest(bupp_basic_io_context_header_${name}_test
                 bupp.basic.io_context.header.${name})
endfunction()

bupp_add_basic_io_context_header_test(buffer BUPP_HEADER_TEST_BUFFER)
bupp_add_basic_io_context_header_test(io_context BUPP_HEADER_TEST_IO_CONTEXT)
bupp_add_basic_io_context_header_test(io_context_cpo
                                      BUPP_HEADER_TEST_IO_CONTEXT_CPO)
bupp_add_basic_io_context_header_test(ip BUPP_HEADER_TEST_IP)
bupp_add_basic_io_context_header_test(ssl BUPP_HEADER_TEST_SSL)
bupp_add_basic_io_context_header_test(tcp BUPP_HEADER_TEST_TCP)
bupp_add_basic_io_context_header_test(udp BUPP_HEADER_TEST_UDP)
