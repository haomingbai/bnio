find_package(Threads REQUIRED)

function(bnio_add_stress_async_io_test name source)
  add_executable(bnio_stress_async_io_${name}_test ${source})
  target_link_libraries(bnio_stress_async_io_${name}_test PRIVATE bnio::bnio
                                                                   Threads::Threads)
  bnio_add_gtest(bnio_stress_async_io_${name}_test
                 bnio.stress.async_io.${name}
                 PROPERTIES LABELS "stress;async_io;${name}" ${ARGN})
endfunction()

bnio_add_stress_async_io_test(rearm_stop rearm_stop_stress_test.cpp
                              PROPERTIES TIMEOUT 120)
