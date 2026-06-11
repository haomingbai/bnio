cmake_minimum_required(VERSION 3.20)

project(
  bupp
  DESCRIPTION "A small io_uring wrapper"
  LANGUAGES CXX)

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/bexec.cmake")

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.2)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BUPP_BUILD_EXAMPLES "Build bupp examples" ON)
option(BUPP_ENABLE_WARNINGS "Enable compiler warnings for bupp targets" ON)

bupp_resolve_bexec_dependency()

function(bupp_set_project_warnings target)
  if(NOT BUPP_ENABLE_WARNINGS)
    return()
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endfunction()

add_subdirectory(include)
add_subdirectory(src)

if(BUPP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

configure_package_config_file(
  ${PROJECT_SOURCE_DIR}/cmake/buppConfig.cmake.in
  ${PROJECT_BINARY_DIR}/buppConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bupp)

install(
  FILES ${PROJECT_BINARY_DIR}/buppConfig.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bupp)
