add_subdirectory(async_io)

if(BUPP_PLATFORM_LINUX)
  add_subdirectory(base)
endif()

if(BUPP_PLATFORM_BSD)
  add_subdirectory(base_bsd)
endif()

if(BUPP_PLATFORM_LINUX OR BUPP_PLATFORM_BSD)
  add_subdirectory(io_context)
endif()
