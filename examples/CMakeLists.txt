if(BNIO_PLATFORM_LINUX)
  add_subdirectory(base/linux)
endif()

if(BNIO_PLATFORM_LINUX OR BNIO_PLATFORM_BSD)
  add_subdirectory(mini_curl)
endif()

if(BNIO_BUILD_ASIO_EXAMPLES)
  add_subdirectory(asio_echo)
endif()
