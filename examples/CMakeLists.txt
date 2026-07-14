if(BUPP_PLATFORM_LINUX)
  add_subdirectory(base/linux)
endif()

if(BUPP_PLATFORM_LINUX OR BUPP_PLATFORM_BSD)
  add_subdirectory(raw_echo)
  add_subdirectory(mini_curl)
endif()

if(BUPP_BUILD_ASIO_EXAMPLES)
  add_subdirectory(asio_echo)
endif()
