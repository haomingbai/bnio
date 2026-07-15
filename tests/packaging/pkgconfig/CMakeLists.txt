cmake_minimum_required(VERSION 3.20)

project(bupp_pkgconfig_consumer LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(BUPP REQUIRED IMPORTED_TARGET bupp>=0.0.1)

add_executable(bupp_pkgconfig_consumer ../consumer.cpp)
target_compile_features(bupp_pkgconfig_consumer PRIVATE cxx_std_20)
target_link_libraries(bupp_pkgconfig_consumer PRIVATE PkgConfig::BUPP)

enable_testing()
add_test(NAME bupp_pkgconfig_consumer COMMAND bupp_pkgconfig_consumer)
