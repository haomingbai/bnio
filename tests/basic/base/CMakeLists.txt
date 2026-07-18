function(bnio_add_basic_base_test name source)
  add_executable(bnio_basic_base_${name}_test ${source})
  target_link_libraries(bnio_basic_base_${name}_test PRIVATE bnio::bnio)
  bnio_add_gtest(bnio_basic_base_${name}_test bnio.basic.base.${name}
                 PROPERTIES LABELS "basic;base;${name}" ${ARGN})
endfunction()

bnio_add_basic_base_test(params params_test.cpp)
bnio_add_basic_base_test(completion_queue_entry
                         completion_queue_entry_test.cpp)
bnio_add_basic_base_test(submission_queue_entry
                         submission_queue_entry_test.cpp)
bnio_add_basic_base_test(probe probe_test.cpp)

function(bnio_add_basic_base_header_test name definition)
  add_executable(bnio_basic_base_header_${name}_test
                 header_self_contained_test.cpp)
  target_compile_definitions(bnio_basic_base_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bnio_basic_base_header_${name}_test PRIVATE bnio::bnio)
  bnio_add_gtest(bnio_basic_base_header_${name}_test
                 bnio.basic.base.header.${name})
endfunction()

bnio_add_basic_base_header_test(base BNIO_HEADER_TEST_BASE)
bnio_add_basic_base_header_test(completion_queue_entry
                                BNIO_HEADER_TEST_COMPLETION_QUEUE_ENTRY)
bnio_add_basic_base_header_test(params BNIO_HEADER_TEST_PARAMS)
bnio_add_basic_base_header_test(probe BNIO_HEADER_TEST_PROBE)
bnio_add_basic_base_header_test(ring BNIO_HEADER_TEST_RING)
bnio_add_basic_base_header_test(submission_queue_entry
                                BNIO_HEADER_TEST_SUBMISSION_QUEUE_ENTRY)
bnio_add_basic_base_header_test(bnio BNIO_HEADER_TEST_BNIO)
