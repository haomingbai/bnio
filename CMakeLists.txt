cmake_minimum_required(VERSION 3.20)

set(BUPP_VERSION "0.0.2" CACHE STRING "bupp package version")

project(
  bupp
  VERSION "${BUPP_VERSION}"
  DESCRIPTION "A small native asynchronous I/O library"
  LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/bexec.cmake")

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(BUPP_IS_TOP_LEVEL ON)
else()
  set(BUPP_IS_TOP_LEVEL OFF)
endif()

# ---- Platform detection ----
#
# bupp uses io_uring on Linux and kqueue on Darwin/FreeBSD. Platform-native
# headers and dependencies are compiled only for their matching backend.
set(BUPP_PLATFORM_LINUX_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(BUPP_PLATFORM_LINUX_DEFAULT ON)
endif()
option(BUPP_PLATFORM_LINUX "Build the Linux io_uring backend"
       ${BUPP_PLATFORM_LINUX_DEFAULT})

set(BUPP_PLATFORM_BSD_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(BUPP_PLATFORM_BSD_DEFAULT ON)
endif()
option(BUPP_PLATFORM_BSD "Build the BSD kqueue backend"
       ${BUPP_PLATFORM_BSD_DEFAULT})

if(BUPP_PLATFORM_LINUX OR BUPP_PLATFORM_BSD)
  find_package(OpenSSL REQUIRED)
endif()

if(BUPP_PLATFORM_LINUX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.1)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BUPP_BUILD_TESTS "Build bupp tests" ${BUPP_IS_TOP_LEVEL})
option(BUPP_BUILD_EXAMPLES "Build bupp examples" ${BUPP_IS_TOP_LEVEL})
option(BUPP_BUILD_ASIO_EXAMPLES "Build standalone Asio echo server example (auto-fetches Asio)" OFF)
option(BUPP_ENABLE_WARNINGS "Enable compiler warnings for bupp targets" ON)
option(BUPP_ENABLE_TSAN "Build bupp targets with ThreadSanitizer" OFF)
option(BUPP_ENABLE_COVERAGE "Instrument bupp targets for coverage reporting" OFF)
option(BUPP_INSTALL "Generate bupp installation rules" ${BUPP_IS_TOP_LEVEL})
option(BUPP_PACKAGE "Generate CPack packaging rules" OFF)

if(BUPP_INSTALL)
  set(
    BUPP_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/bupp"
    CACHE STRING "Directory in which to install the bupp CMake package")
endif()

if(BUPP_BUILD_TESTS)
  include(CTest)
  enable_testing()
endif()

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

if(BUPP_ENABLE_TSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
  else()
    message(FATAL_ERROR "BUPP_ENABLE_TSAN requires GCC or Clang")
  endif()
endif()

if(BUPP_ENABLE_COVERAGE)
  if(BUPP_ENABLE_TSAN)
    message(FATAL_ERROR
            "BUPP_ENABLE_COVERAGE cannot be combined with BUPP_ENABLE_TSAN")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
  else()
    message(FATAL_ERROR
            "BUPP_ENABLE_COVERAGE requires GCC, Clang, or AppleClang")
  endif()
endif()

add_subdirectory(include)
add_subdirectory(src)

if(BUPP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BUPP_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BUPP_INSTALL)
  configure_package_config_file(
    cmake/buppConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/buppConfig.cmake"
    INSTALL_DESTINATION "${BUPP_INSTALL_CMAKEDIR}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/buppConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/buppConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/buppConfigVersion.cmake"
    DESTINATION "${BUPP_INSTALL_CMAKEDIR}"
    COMPONENT development)

  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    file(
      RELATIVE_PATH BUPP_PC_PREFIX_RELATIVE
      "${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
  else()
    file(
      RELATIVE_PATH BUPP_PC_PREFIX_RELATIVE
      "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
  endif()

  set(BUPP_PC_REQUIRES "bexec >= ${BUPP_BEXEC_MIN_VERSION}, openssl")
  if(BUPP_PLATFORM_LINUX)
    string(APPEND BUPP_PC_REQUIRES ", liburing >= 2.1")
  endif()
  if(BUILD_SHARED_LIBS)
    set(BUPP_PC_SHARED_CFLAGS " -DBUPP_SHARED_LIBRARY")
  else()
    set(BUPP_PC_SHARED_CFLAGS "")
  endif()
  configure_file(cmake/bupp.pc.in "${CMAKE_CURRENT_BINARY_DIR}/bupp.pc"
                 @ONLY)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/bupp.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT development)
  install(FILES LICENSE README.md
          DESTINATION "${CMAKE_INSTALL_DOCDIR}"
          COMPONENT development)
endif()

# ---- CPack packaging ----
if(BUPP_PACKAGE)
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/cpack.cmake")
endif()
