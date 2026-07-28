if(BNIO_PLATFORM_LINUX OR BNIO_PLATFORM_BSD)
  add_subdirectory(io_context)
endif()

if(BNIO_PLATFORM_BSD)
  add_subdirectory(base_bsd)
endif()
