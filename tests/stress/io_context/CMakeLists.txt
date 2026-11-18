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
bnio_add_stress_io_context_test(local_write_peer_close
                                local_write_peer_close_stress_test.cpp
                                PROPERTIES TIMEOUT 30)
bnio_add_stress_io_context_test(shutdown_submit shutdown_submit_stress_test.cpp
                                PROPERTIES TIMEOUT 60)
bnio_add_stress_io_context_test(timer_stop_race timer_stop_race_stress_test.cpp
                                PROPERTIES TIMEOUT 60)
bnio_add_stress_io_context_test(ssl_handshake_retry_stress
                                ssl_handshake_retry_stress_test.cpp
                                PROPERTIES TIMEOUT 60)
bnio_add_stress_io_context_test(ssl_callback_stress
                                ssl_callback_stress_test.cpp
                                PROPERTIES TIMEOUT 120)
# The stress test moves and destroys callback owners across handshakes and
# asserts no closure runs after its context is gone; a use-after-free is only
# observable under AddressSanitizer (same pattern as ssl_alpn).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT BNIO_ENABLE_TSAN)
  target_compile_options(
    bnio_stress_io_context_ssl_callback_stress_test
    PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(
    bnio_stress_io_context_ssl_callback_stress_test
    PRIVATE -fsanitize=address)
endif()
