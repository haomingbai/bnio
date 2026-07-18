cmake_minimum_required(VERSION 3.20)

project(bnio_cmake_consumer LANGUAGES CXX)

find_package(bnio 0.0.1 CONFIG REQUIRED)

add_executable(bnio_cmake_consumer ../consumer.cpp)
target_link_libraries(bnio_cmake_consumer PRIVATE bnio::bnio)

enable_testing()
add_test(NAME bnio_cmake_consumer COMMAND bnio_cmake_consumer)
