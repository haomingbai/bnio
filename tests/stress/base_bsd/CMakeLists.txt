function(bnio_add_stress_base_bsd_test name source)
  add_executable(bnio_stress_base_bsd_${name}_test ${source})
  target_link_libraries(bnio_stress_base_bsd_${name}_test PRIVATE bnio::bnio)
  bnio_add_gtest(bnio_stress_base_bsd_${name}_test
                 bnio.stress.base_bsd.${name}
                 PROPERTIES LABELS "stress;base_bsd;${name}" ${ARGN})
endfunction()

bnio_add_stress_base_bsd_test(wake_channel wake_channel_stress_test.cpp
                              PROPERTIES TIMEOUT 10)
