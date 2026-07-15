cmake_minimum_required(VERSION 3.20)

project(bupp_cmake_consumer LANGUAGES CXX)

find_package(bupp 0.0.1 CONFIG REQUIRED)

add_executable(bupp_cmake_consumer ../consumer.cpp)
target_link_libraries(bupp_cmake_consumer PRIVATE bupp::bupp)

enable_testing()
add_test(NAME bupp_cmake_consumer COMMAND bupp_cmake_consumer)
