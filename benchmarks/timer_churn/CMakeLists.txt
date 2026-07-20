find_package(Threads REQUIRED)

add_executable(bnio_timer_churn_benchmark bnio_timer_churn_benchmark.cpp)
target_link_libraries(bnio_timer_churn_benchmark PRIVATE bnio::bnio
                                                        Threads::Threads)
bnio_set_project_warnings(bnio_timer_churn_benchmark)

add_executable(asio_timer_churn_benchmark asio_timer_churn_benchmark.cpp)
target_compile_features(asio_timer_churn_benchmark PRIVATE cxx_std_20)
target_compile_definitions(asio_timer_churn_benchmark PRIVATE ASIO_STANDALONE)
target_link_libraries(asio_timer_churn_benchmark PRIVATE asio::asio
                                                        Threads::Threads)
bnio_set_project_warnings(asio_timer_churn_benchmark)
