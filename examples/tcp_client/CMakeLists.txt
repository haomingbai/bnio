add_executable(bnio_tcp_client tcp_client.cpp)
target_link_libraries(bnio_tcp_client PRIVATE bnio::bnio)
