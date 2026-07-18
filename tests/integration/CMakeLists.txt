add_subdirectory(async_io)

if(BNIO_PLATFORM_LINUX)
  add_subdirectory(base)
endif()

if(BNIO_PLATFORM_BSD)
  add_subdirectory(base_bsd)
endif()

if(BNIO_PLATFORM_LINUX OR BNIO_PLATFORM_BSD)
  add_subdirectory(io_context)
endif()
