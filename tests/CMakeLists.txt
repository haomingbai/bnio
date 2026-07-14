add_subdirectory(async_io)

if(BUPP_PLATFORM_LINUX)
  add_subdirectory(base/linux)
endif()

if(BUPP_PLATFORM_LINUX OR BUPP_PLATFORM_BSD)
  add_subdirectory(io_context)
endif()

if(BUPP_PLATFORM_BSD)
  add_subdirectory(base/bsd)
endif()
