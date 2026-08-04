find_package(Threads REQUIRED)

function(bnio_add_stress_io_context_test name source)
  add_executable(bnio_stress_io_context_${name}_test ${source})
  target_link_libraries(bnio_stress_io_context_${name}_test PRIVATE bnio::bnio
                                                                   Threads::Threads)
  bnio_add_gtest(bnio_stress_io_context_${name}_test
                 bnio.stress.io_context.${name}
                 PROPERTIES LABELS "stress;io_context;${name}" ${ARGN})
endfunction()

bnio_add_stress_io_context_test(udp_stress udp_stress_test.cpp
                                PROPERTIES TIMEOUT 10)
bnio_add_stress_io_context_test(tcp_stress tcp_stress_test.cpp
                                PROPERTIES TIMEOUT 10)
bnio_add_stress_io_context_test(timer_stress timer_stress_test.cpp
                                PROPERTIES TIMEOUT 10)
bnio_add_stress_io_context_test(read_write_stress read_write_stress_test.cpp
                                PROPERTIES TIMEOUT 10)
bnio_add_stress_io_context_test(shutdown_submit shutdown_submit_stress_test.cpp
                                PROPERTIES TIMEOUT 60)
bnio_add_stress_io_context_test(timer_stop_race timer_stop_race_stress_test.cpp
                                PROPERTIES TIMEOUT 60)
