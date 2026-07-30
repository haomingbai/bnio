add_subdirectory(posix)

if(BNIO_PLATFORM_LINUX)
  add_subdirectory(linux)
endif()

if(BNIO_PLATFORM_BSD)
  add_subdirectory(bsd)
endif()
