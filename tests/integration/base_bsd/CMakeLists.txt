function(bupp_add_integration_base_bsd_test name source)
  add_executable(bupp_integration_base_bsd_${name}_test ${source})
  target_link_libraries(bupp_integration_base_bsd_${name}_test PRIVATE
                        bupp::bupp)
  bupp_add_gtest(bupp_integration_base_bsd_${name}_test
                 bupp.integration.base_bsd.${name}
                 PROPERTIES LABELS "integration;base_bsd;${name}" ${ARGN})
endfunction()

bupp_add_integration_base_bsd_test(kqueue kqueue_test.cpp)
