function(bupp_add_bsd_base_test name source)
  add_executable(bupp_base_bsd_${name}_test ${source})
  target_link_libraries(bupp_base_bsd_${name}_test PRIVATE bupp::bupp)
  bupp_set_project_warnings(bupp_base_bsd_${name}_test)
  add_test(NAME bupp.base.bsd.${name} COMMAND bupp_base_bsd_${name}_test)
endfunction()

bupp_add_bsd_base_test(kqueue kqueue_test.cpp)

function(bupp_add_bsd_base_header_self_contained_test name definition)
  add_executable(bupp_base_bsd_header_${name}_test
                 header_self_contained_test.cpp)
  target_compile_definitions(bupp_base_bsd_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bupp_base_bsd_header_${name}_test PRIVATE bupp::bupp)
  bupp_set_project_warnings(bupp_base_bsd_header_${name}_test)
  add_test(NAME bupp.base.bsd.header.${name}
           COMMAND bupp_base_bsd_header_${name}_test)
endfunction()

bupp_add_bsd_base_header_self_contained_test(base BUPP_HEADER_TEST_BASE)
bupp_add_bsd_base_header_self_contained_test(event BUPP_HEADER_TEST_EVENT)
bupp_add_bsd_base_header_self_contained_test(event_list_view
                                            BUPP_HEADER_TEST_EVENT_LIST_VIEW)
bupp_add_bsd_base_header_self_contained_test(kqueue BUPP_HEADER_TEST_KQUEUE)
bupp_add_bsd_base_header_self_contained_test(bupp BUPP_HEADER_TEST_BUPP)
