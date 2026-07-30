add_executable(bnio_udp_echo udp_echo.cpp)
target_link_libraries(bnio_udp_echo PRIVATE bnio::bnio)
