function(bnio_add_integration_base_bsd_test name source)
  add_executable(bnio_integration_base_bsd_${name}_test ${source})
  target_link_libraries(bnio_integration_base_bsd_${name}_test PRIVATE
                        bnio::bnio)
  bnio_add_gtest(bnio_integration_base_bsd_${name}_test
                 bnio.integration.base_bsd.${name}
                 PROPERTIES LABELS "integration;base_bsd;${name}" ${ARGN})
endfunction()

bnio_add_integration_base_bsd_test(kqueue kqueue_test.cpp)
bnio_add_integration_base_bsd_test(wake_channel wake_channel_test.cpp)
