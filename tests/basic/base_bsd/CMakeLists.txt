function(bupp_add_basic_base_bsd_header_test name definition)
  add_executable(bupp_basic_base_bsd_header_${name}_test
                 header_self_contained_test.cpp)
  target_compile_definitions(bupp_basic_base_bsd_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bupp_basic_base_bsd_header_${name}_test PRIVATE
                        bupp::bupp)
  bupp_add_gtest(bupp_basic_base_bsd_header_${name}_test
                 bupp.basic.base_bsd.header.${name})
endfunction()

bupp_add_basic_base_bsd_header_test(base BUPP_HEADER_TEST_BASE)
bupp_add_basic_base_bsd_header_test(event BUPP_HEADER_TEST_EVENT)
bupp_add_basic_base_bsd_header_test(event_list_view
                                    BUPP_HEADER_TEST_EVENT_LIST_VIEW)
bupp_add_basic_base_bsd_header_test(kqueue BUPP_HEADER_TEST_KQUEUE)
bupp_add_basic_base_bsd_header_test(bupp BUPP_HEADER_TEST_BUPP)
