cmake_minimum_required(VERSION 3.20)

project(bnio_source_consumer LANGUAGES CXX)

if(NOT BNIO_SOURCE_DIR)
  message(FATAL_ERROR "BNIO_SOURCE_DIR must point to the bnio source tree")
endif()

add_subdirectory("${BNIO_SOURCE_DIR}" bnio)

add_executable(bnio_source_consumer ../consumer.cpp)
target_link_libraries(bnio_source_consumer PRIVATE bnio::bnio)

enable_testing()
add_test(NAME bnio_source_consumer COMMAND bnio_source_consumer)
