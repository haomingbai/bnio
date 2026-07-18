cmake_minimum_required(VERSION 3.20)

project(bnio_pkgconfig_consumer LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(BNIO REQUIRED IMPORTED_TARGET bnio>=0.0.1)

add_executable(bnio_pkgconfig_consumer ../consumer.cpp)
target_compile_features(bnio_pkgconfig_consumer PRIVATE cxx_std_20)
target_link_libraries(bnio_pkgconfig_consumer PRIVATE PkgConfig::BNIO)

enable_testing()
add_test(NAME bnio_pkgconfig_consumer COMMAND bnio_pkgconfig_consumer)
