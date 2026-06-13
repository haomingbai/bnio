find_package(Threads REQUIRED)

# Resolve Asio — try system package first, fall back to FetchContent
if(NOT TARGET asio::asio)
  find_package(asio CONFIG QUIET)
  if(NOT asio_FOUND)
    # Call the shared resolution function defined in the root CMakeLists.txt.
    # It will FetchContent asio if needed.
    if(COMMAND bupp_resolve_asio_dependency)
      bupp_resolve_asio_dependency()
    endif()
  endif()
endif()

# If still not found, try find_path as a last resort
if(NOT TARGET asio::asio)
  find_path(ASIO_INCLUDE_DIR asio.hpp)
  if(NOT ASIO_INCLUDE_DIR)
    message(
      FATAL_ERROR
        "BUPP_BUILD_BENCHMARKS=ON requires standalone Asio headers. "
        "Install libasio-dev, pass -DASIO_INCLUDE_DIR=/path/to/asio/include, "
        "or enable -DBUPP_BUILD_ASIO_EXAMPLES=ON to auto-fetch.")
  endif()
endif()

# ---- bupp HTTP echo server (same source as the io_context example) ----
add_executable(
  bupp_benchmark_bupp_http_echo_server
  ../io_context/http_echo_server/http_echo_server.cpp)
target_link_libraries(bupp_benchmark_bupp_http_echo_server PRIVATE bupp::bupp)
bupp_set_project_warnings(bupp_benchmark_bupp_http_echo_server)

# ---- Asio HTTP echo server (standalone Asio, no Beast) ----
add_executable(bupp_benchmark_asio_http_echo_server asio_http_echo_server.cpp)
target_compile_features(bupp_benchmark_asio_http_echo_server PRIVATE cxx_std_20)
target_compile_definitions(bupp_benchmark_asio_http_echo_server
                           PRIVATE ASIO_STANDALONE)
target_link_libraries(bupp_benchmark_asio_http_echo_server
                      PRIVATE Threads::Threads)

if(TARGET asio::asio)
  target_link_libraries(bupp_benchmark_asio_http_echo_server PRIVATE asio::asio)
else()
  target_include_directories(bupp_benchmark_asio_http_echo_server
                             SYSTEM PRIVATE ${ASIO_INCLUDE_DIR})
endif()
