function(bupp_add_integration_base_test name source)
  add_executable(bupp_integration_base_${name}_test ${source})
  target_link_libraries(bupp_integration_base_${name}_test PRIVATE bupp::bupp)
  bupp_add_gtest(bupp_integration_base_${name}_test
                 bupp.integration.base.${name}
                 PROPERTIES LABELS "integration;base;${name}" ${ARGN})
endfunction()

bupp_add_integration_base_test(ring ring_test.cpp PROPERTIES TIMEOUT 10)
