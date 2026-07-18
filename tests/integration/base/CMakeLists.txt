function(bnio_add_integration_base_test name source)
  add_executable(bnio_integration_base_${name}_test ${source})
  target_link_libraries(bnio_integration_base_${name}_test PRIVATE bnio::bnio)
  bnio_add_gtest(bnio_integration_base_${name}_test
                 bnio.integration.base.${name}
                 PROPERTIES LABELS "integration;base;${name}" ${ARGN})
endfunction()

bnio_add_integration_base_test(ring ring_test.cpp PROPERTIES TIMEOUT 10)
