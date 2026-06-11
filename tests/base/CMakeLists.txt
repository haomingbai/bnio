function(bupp_add_base_test name source)
  add_executable(bupp_base_${name}_test ${source})
  target_link_libraries(bupp_base_${name}_test PRIVATE bupp::bupp)
  bupp_set_project_warnings(bupp_base_${name}_test)
  add_test(NAME bupp.base.${name} COMMAND bupp_base_${name}_test)
endfunction()

bupp_add_base_test(params params_test.cpp)
bupp_add_base_test(completion_queue_entry completion_queue_entry_test.cpp)
bupp_add_base_test(submission_queue_entry submission_queue_entry_test.cpp)
bupp_add_base_test(ring ring_test.cpp)
bupp_add_base_test(probe probe_test.cpp)

function(bupp_add_base_header_self_contained_test name definition)
  add_executable(bupp_base_header_${name}_test header_self_contained_test.cpp)
  target_compile_definitions(bupp_base_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bupp_base_header_${name}_test PRIVATE bupp::bupp)
  bupp_set_project_warnings(bupp_base_header_${name}_test)
  add_test(NAME bupp.base.header.${name}
           COMMAND bupp_base_header_${name}_test)
endfunction()

bupp_add_base_header_self_contained_test(base BUPP_HEADER_TEST_BASE)
bupp_add_base_header_self_contained_test(
  completion_queue_entry BUPP_HEADER_TEST_COMPLETION_QUEUE_ENTRY)
bupp_add_base_header_self_contained_test(params BUPP_HEADER_TEST_PARAMS)
bupp_add_base_header_self_contained_test(probe BUPP_HEADER_TEST_PROBE)
bupp_add_base_header_self_contained_test(ring BUPP_HEADER_TEST_RING)
bupp_add_base_header_self_contained_test(
  submission_queue_entry BUPP_HEADER_TEST_SUBMISSION_QUEUE_ENTRY)
bupp_add_base_header_self_contained_test(bupp BUPP_HEADER_TEST_BUPP)
