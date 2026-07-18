function(bnio_add_basic_base_bsd_header_test name definition)
  add_executable(bnio_basic_base_bsd_header_${name}_test
                 header_self_contained_test.cpp)
  target_compile_definitions(bnio_basic_base_bsd_header_${name}_test
                             PRIVATE ${definition})
  target_link_libraries(bnio_basic_base_bsd_header_${name}_test PRIVATE
                        bnio::bnio)
  bnio_add_gtest(bnio_basic_base_bsd_header_${name}_test
                 bnio.basic.base_bsd.header.${name})
endfunction()

bnio_add_basic_base_bsd_header_test(base BNIO_HEADER_TEST_BASE)
bnio_add_basic_base_bsd_header_test(event BNIO_HEADER_TEST_EVENT)
bnio_add_basic_base_bsd_header_test(event_list_view
                                    BNIO_HEADER_TEST_EVENT_LIST_VIEW)
bnio_add_basic_base_bsd_header_test(kqueue BNIO_HEADER_TEST_KQUEUE)
bnio_add_basic_base_bsd_header_test(bnio BNIO_HEADER_TEST_BNIO)
