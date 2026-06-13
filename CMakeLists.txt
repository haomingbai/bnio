cmake_minimum_required(VERSION 3.20)

project(
  bupp
  DESCRIPTION "A small io_uring wrapper"
  LANGUAGES CXX)

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/bexec.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/wrk.cmake")

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.2)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BUPP_BUILD_EXAMPLES "Build bupp examples" ON)
option(BUPP_BUILD_BENCHMARKS "Build optional bupp benchmark examples" OFF)
option(BUPP_BUILD_ASIO_EXAMPLES "Build standalone Asio echo server example (auto-fetches Asio)" OFF)
option(BUPP_FETCH_WRK "Auto-fetch and build wrk benchmarking tool" OFF)
option(BUPP_ENABLE_WARNINGS "Enable compiler warnings for bupp targets" ON)

bupp_resolve_bexec_dependency()

# ---- Standalone Asio dependency resolution ----
#
# Call bupp_resolve_asio_dependency() to ensure asio::asio is available.
# Tries find_package first; falls back to FetchContent from GitHub.

function(bupp_resolve_asio_dependency)
  if(TARGET asio::asio)
    return()
  endif()

  include(FetchContent)

  find_package(asio CONFIG QUIET)
  if(asio_FOUND)
    message(STATUS "Using system asio (found via find_package)")
    return()
  endif()

  message(STATUS "Fetching standalone Asio from GitHub...")
  FetchContent_Declare(
    asio_fetch
    GIT_REPOSITORY "https://github.com/chriskohlhoff/asio.git"
    GIT_TAG "asio-1-30-2"
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(asio_fetch)

  if(NOT TARGET asio::asio)
    if(TARGET asio)
      add_library(asio::asio ALIAS asio)
    else()
      add_library(asio INTERFACE)
      add_library(asio::asio ALIAS asio)
      target_include_directories(asio INTERFACE
                                 "${asio_fetch_SOURCE_DIR}/asio/include")
      target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
    endif()
  endif()
  message(STATUS "Asio fetched successfully (asio::asio)")
endfunction()

if(BUPP_BUILD_ASIO_EXAMPLES)
  bupp_resolve_asio_dependency()
endif()

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

if(BUPP_BUILD_BENCHMARKS)
  add_subdirectory(examples/benchmark)
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
