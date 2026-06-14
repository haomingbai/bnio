add_subdirectory(base/linux)
add_subdirectory(raw_echo)
add_subdirectory(mini_curl)

if(BUPP_BUILD_ASIO_EXAMPLES)
  add_subdirectory(asio_echo)
endif()
