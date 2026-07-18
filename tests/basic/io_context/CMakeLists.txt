find_package(Threads REQUIRED)

function(bnio_add_basic_io_context_test name source)
  add_executable(bnio_basic_io_context_${name}_test ${source})
  target_link_libraries(bnio_basic_io_context_${name}_test PRIVATE bnio::bnio
                                                                Threads::Threads)
  bnio_add_gtest(bnio_basic_io_context_${name}_test bnio.basic.io_context.${name}
                 PROPERTIES LABELS "basic;io_context;${name}" ${ARGN})
endfunction()

bnio_add_basic_io_context_test(buffer buffer_test.cpp)
bnio_add_basic_io_context_test(io_context_sender_concept
                               io_context_sender_concept_test.cpp)
bnio_add_basic_io_context_test(ssl_concept ssl_concept_test.cpp)

function(bnio_add_basic_io_context_header_test name definition)
  add_executable(bnio_basic_io_context_header_${name}_test
                 header_self_contained_test.cpp)
  target_compile_definitions(bnio_basic_io_context_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bnio_basic_io_context_header_${name}_test PRIVATE
                        bnio::bnio)
  bnio_add_gtest(bnio_basic_io_context_header_${name}_test
                 bnio.basic.io_context.header.${name})
endfunction()

bnio_add_basic_io_context_header_test(buffer BNIO_HEADER_TEST_BUFFER)
bnio_add_basic_io_context_header_test(io_context BNIO_HEADER_TEST_IO_CONTEXT)
bnio_add_basic_io_context_header_test(io_context_cpo
                                      BNIO_HEADER_TEST_IO_CONTEXT_CPO)
bnio_add_basic_io_context_header_test(ip BNIO_HEADER_TEST_IP)
bnio_add_basic_io_context_header_test(ssl BNIO_HEADER_TEST_SSL)
bnio_add_basic_io_context_header_test(tcp BNIO_HEADER_TEST_TCP)
bnio_add_basic_io_context_header_test(udp BNIO_HEADER_TEST_UDP)
