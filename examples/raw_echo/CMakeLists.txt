# ---- bnio raw echo server ----
add_executable(bnio_raw_echo bnio_raw_echo.cpp)
target_link_libraries(bnio_raw_echo PRIVATE bnio::bnio)
bnio_set_project_warnings(bnio_raw_echo)

if(BNIO_BUILD_ASIO_EXAMPLES)
  # ---- asio raw echo server (for comparison) ----
  add_executable(asio_raw_echo asio_raw_echo.cpp)
  target_compile_features(asio_raw_echo PRIVATE cxx_std_20)
  target_compile_definitions(asio_raw_echo PRIVATE ASIO_STANDALONE)
  target_link_libraries(asio_raw_echo PRIVATE asio::asio)
  bnio_set_project_warnings(asio_raw_echo)

  # ---- benchmark client ----
  add_executable(raw_echo_client client.cpp)
  target_compile_features(raw_echo_client PRIVATE cxx_std_20)
  target_compile_definitions(raw_echo_client PRIVATE ASIO_STANDALONE)
  target_link_libraries(raw_echo_client PRIVATE asio::asio)
  bnio_set_project_warnings(raw_echo_client)
endif()
