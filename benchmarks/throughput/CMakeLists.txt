find_package(Threads REQUIRED)

add_executable(bnio_throughput_benchmark bnio_throughput_benchmark.cpp)
target_link_libraries(bnio_throughput_benchmark PRIVATE bnio::bnio
                                                         Threads::Threads)
bnio_set_project_warnings(bnio_throughput_benchmark)

add_executable(asio_throughput_benchmark asio_throughput_benchmark.cpp)
target_compile_features(asio_throughput_benchmark PRIVATE cxx_std_20)
target_compile_definitions(asio_throughput_benchmark PRIVATE ASIO_STANDALONE)
target_link_libraries(asio_throughput_benchmark PRIVATE asio::asio
                                                         Threads::Threads)
bnio_set_project_warnings(asio_throughput_benchmark)

add_executable(throughput_benchmark_client throughput_benchmark_client.cpp)
target_compile_features(throughput_benchmark_client PRIVATE cxx_std_20)
target_compile_definitions(throughput_benchmark_client PRIVATE ASIO_STANDALONE)
target_link_libraries(throughput_benchmark_client PRIVATE asio::asio
                                                        Threads::Threads)
bnio_set_project_warnings(throughput_benchmark_client)
