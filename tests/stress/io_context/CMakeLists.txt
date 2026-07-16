find_package(Threads REQUIRED)

function(bupp_add_stress_io_context_test name source)
  add_executable(bupp_stress_io_context_${name}_test ${source})
  target_link_libraries(bupp_stress_io_context_${name}_test PRIVATE bupp::bupp
                                                                   Threads::Threads)
  bupp_add_gtest(bupp_stress_io_context_${name}_test
                 bupp.stress.io_context.${name}
                 PROPERTIES LABELS "stress;io_context;${name}" ${ARGN})
endfunction()

bupp_add_stress_io_context_test(udp_stress udp_stress_test.cpp
                                PROPERTIES TIMEOUT 10)
