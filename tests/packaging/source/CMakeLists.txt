cmake_minimum_required(VERSION 3.20)

project(bupp_source_consumer LANGUAGES CXX)

if(NOT BUPP_SOURCE_DIR)
  message(FATAL_ERROR "BUPP_SOURCE_DIR must point to the bupp source tree")
endif()

add_subdirectory("${BUPP_SOURCE_DIR}" bupp)

add_executable(bupp_source_consumer ../consumer.cpp)
target_link_libraries(bupp_source_consumer PRIVATE bupp::bupp)

enable_testing()
add_test(NAME bupp_source_consumer COMMAND bupp_source_consumer)
